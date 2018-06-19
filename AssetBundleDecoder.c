/*
Copyright (c) 2014 - 2018, lujian
All rights reserved.

email: lujian_niewa@sina.com, 345005607@qq.com
*/
#define ABD_LIB
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "Lzma/Alloc.h"
#include "Lzma/7zFile.h"
#include "Lzma/7zVersion.h"
#include "Lzma/LzmaDec.h"
#include "lz4/lz4.h"
#include "AssetBundleDecoder.h"

#ifdef _MSC_VER
#pragma warning( disable : 4996 )
#endif

#define MAX_AB_STRLEN 16
#define TAG_ASSETS_WEB "UnityWeb"
#define TAG_ASSETS_RAW "UnityRaw"
#define TAG_ASSETS_FS "UnityFS"

/*unity's uncompressed assetbundle's header always be compressed.*/
#define KEEP_COMPRESSED_BUNDLE_HEADER 0
/*append 0 if uncompressed file size is smaller than bundleSize declared in header.*/
#define PADDING_UNCOMPRESSED_FILE_SIZE 1

static RequireDiskSpaceHook _Hook = NULL;

enum {
	LZ4_BLOCK_BYTES = 1024 * 8,
};

#define IN_BUF_SIZE		( 1 << 16 )
#define OUT_BUF_SIZE	( 1 << 16 )

#ifndef min
#define min( a, b ) ( (a) < (b) ? (a) : (b) )
#endif

/* list of compressed and uncompressed offsets*/
typedef struct _LevelByteEnd_t {
	Int32 start;
	Int32 end;
} LevelByteEnd_t;

typedef struct _AssetBundleHeader_t {
	char assetType[ MAX_AB_STRLEN ];
	/* file version
	3 in Unity 3.5 and 4
	2 in Unity 2.6 to 3.4
	1 in Unity 1 to 2.5 */
	Int32 streamVersion;
	char unityVersion[ MAX_AB_STRLEN ];
	char unityRevision[ MAX_AB_STRLEN ];
	Int32 minimumStreamedBytes;
	Int32 headerSize;
	Int32 numberOfLevelsToDownload;
	LevelByteEnd_t* levelByteEnd;
	/* equal to file size, sometimes equal to uncompressed data size without the header */
	Int32 completeFileSize;
	/* offset to the first asset file within the data area? equals compressed */
	/* file size if completeFileSize contains the uncompressed data size */
	Int32 dataHeaderSize;
	Int32 numberOfLevels;

} AssetBundleHeader_t;

typedef struct _ChunkBasedBundleHeader_t {
	char assetType[ MAX_AB_STRLEN ];
	Int32 streamVersion;
	char unityVersion[ MAX_AB_STRLEN ];
	char unityRevision[ MAX_AB_STRLEN ];
	Int64 bundleSize;
	Int32 chunkInfoCompressedSize;
	Int32 chunkInfoUncompressedSize;
	Int32 flag;
	Int32 headerSize;
} ChunkBasedBundleHeader_t;

static void _read_string( CSzFile* fp, char* dst, int bufLen ) {
	int ofs = 0;
	char _c = 0;
	size_t size;
	do {
		size = 1;
		if ( File_Read( fp, &_c, &size ) == 0 ) {
			dst[ ofs++ ] = ( char )_c;
		} else {
			break;
		}
	} while ( _c != '\0' && ofs < bufLen );
}

static void _mread_string( const char** p, char* dst, int bufLen ) {
	int ofs = 0;
	char _c = 0;
	do {
		_c = *( *p )++;
		dst[ ofs++ ] = _c;
	} while ( _c != '\0' && ofs < bufLen );
}

static void _write_string( CSzFile* fp, char* src ) {
	size_t size = strlen( src ) + 1;
	File_Write( fp, src, &size );
}

static Int32 _read_int( CSzFile* fp ) {
	Byte bytes[ 4 ];
	size_t size = sizeof( bytes );
	File_Read( fp, bytes, &size );
	assert( size == sizeof( bytes ) );
	return ( bytes[ 0 ] << 24 ) | ( bytes[ 1 ] << 16 ) | ( bytes[ 2 ] << 8 ) | bytes[ 3 ];
}

static Int32 _mread_int( const char** p ) {
	Byte bytes[ 4 ];
	memcpy( bytes, *p, sizeof( bytes ) );
	*p += sizeof( bytes );
	return ( bytes[ 0 ] << 24 ) | ( bytes[ 1 ] << 16 ) | ( bytes[ 2 ] << 8 ) | bytes[ 3 ];
}

static Int16 _mread_int16( const char** p ) {
	Byte bytes[ 2 ];
	memcpy( bytes, *p, sizeof( bytes ) );
	*p += sizeof( bytes );
	return ( bytes[ 0 ] << 8 ) | bytes[ 1 ];
}

static Int64 _read_int64( CSzFile* fp ) {
	Byte bytes[ 8 ];
	size_t size = sizeof( bytes );
	File_Read( fp, bytes, &size );
	assert( size == sizeof( bytes ) );
	return ( ( Int64 )bytes[ 0 ] << 56 ) | ( ( Int64 )bytes[ 1 ] << 48 ) | ( ( Int64 )bytes[ 2 ] << 40 ) | ( ( Int64 )bytes[ 3 ] << 32 ) |
		( ( Int64 )bytes[ 4 ] << 24 ) | ( ( Int64 )bytes[ 5 ] << 16 ) | ( ( Int64 )bytes[ 6 ] << 8 ) | ( Int64 )bytes[ 7 ];
}

static Int64 _mread_int64( const char** fp ) {
	Byte bytes[ 8 ];
	memcpy( bytes, *fp, sizeof( bytes ) );
	*fp += sizeof( bytes );
	return ( ( Int64 )bytes[ 0 ] << 56 ) | ( ( Int64 )bytes[ 1 ] << 48 ) | ( ( Int64 )bytes[ 2 ] << 40 ) | ( ( Int64 )bytes[ 3 ] << 32 ) |
		( ( Int64 )bytes[ 4 ] << 24 ) | ( ( Int64 )bytes[ 5 ] << 16 ) | ( ( Int64 )bytes[ 6 ] << 8 ) | ( Int64 )bytes[ 7 ];
}

static void _write_int( CSzFile* fp, Int32 value ) {
	Byte* bytes = ( Byte* )&value;
	size_t size = sizeof( Int32 );
	Int32 _value = ( bytes[ 0 ] << 24 ) | ( bytes[ 1 ] << 16 ) | ( bytes[ 2 ] << 8 ) | bytes[ 3 ];
	File_Write( fp, &_value, &size );
}

static void _write_int64( CSzFile* fp, Int64 value ) {
	Byte* bytes = ( Byte* )&value;
	size_t size = sizeof( Int64 );
	Int64 _value = ( ( Int64 )bytes[ 0 ] << 56 ) | ( ( Int64 )bytes[ 1 ] << 48 ) | ( ( Int64 )bytes[ 2 ] << 40 ) | ( ( Int64 )bytes[ 3 ] << 32 ) |
		( ( Int64 )bytes[ 4 ] << 24 ) | ( ( Int64 )bytes[ 5 ] << 16 ) | ( ( Int64 )bytes[ 6 ] << 8 ) | ( Int64 )bytes[ 7 ];
	File_Write( fp, &_value, &size );
}

static Byte _read_byte( CSzFile* fp ) {
	Byte c;
	size_t size = 1;
	File_Read( fp, &c, &size );
	return c;
}

static void _write_byte( CSzFile* fp, Byte value ) {
	size_t size = 1;
	File_Write( fp, &value, &size );
}

static void AB_DestroyHeaderData( AssetBundleHeader_t* header ) {
	if ( header != NULL ) {
		if ( header->levelByteEnd != NULL ) {
			free( header->levelByteEnd );
			header->levelByteEnd = NULL;
		}
	}
}

static Int32 AB_ReadFormatFromFile( CSzFile* fp ) {
	Int64 pos = 0;
	Int32 format;
	Int64 beginOffset = 0;
	char assetType[ MAX_AB_STRLEN ];
	File_Seek( fp, &pos, SZ_SEEK_CUR );
	if ( beginOffset != pos ) {
		File_Seek( fp, &beginOffset, SZ_SEEK_SET );
	}
	_read_string( fp, assetType, sizeof( assetType ) );
	format = _read_int( fp );
	File_Seek( fp, &pos, SZ_SEEK_SET );
	return format;
}

static void AB_ReadChunkBasedBundleHeader( ChunkBasedBundleHeader_t* header, CSzFile* fp ) {
	Int64 curPos = 0;
	_read_string( fp, header->assetType, sizeof( header->assetType ) );
	header->streamVersion = _read_int( fp );
	_read_string( fp, header->unityVersion, sizeof( header->unityVersion ) );
	_read_string( fp, header->unityRevision, sizeof( header->unityRevision ) );
	header->bundleSize = _read_int64( fp );
	header->chunkInfoCompressedSize = _read_int( fp );
	header->chunkInfoUncompressedSize = _read_int( fp );
	header->flag = _read_int( fp );
	File_Seek( fp, &curPos, SZ_SEEK_CUR );
	header->headerSize = ( Int32 )curPos;
}

static void AB_WriteChunkBasedBundleHeader( ChunkBasedBundleHeader_t* header, CSzFile* fp ) {
	_write_string( fp, header->assetType );
	_write_int( fp, header->streamVersion );
	_write_string( fp, header->unityVersion );
	_write_string( fp, header->unityRevision );
	_write_int64( fp, header->bundleSize );
	_write_int( fp, header->chunkInfoCompressedSize );
	_write_int( fp, header->chunkInfoUncompressedSize );
	_write_int( fp, header->flag );
}

static void AB_ReadHeaderFromFile( AssetBundleHeader_t* header, CSzFile* fp ) {
	_read_string( fp, header->assetType, sizeof( header->assetType ) );
	header->streamVersion = _read_int( fp );
	_read_string( fp, header->unityVersion, sizeof( header->unityVersion ) );
	_read_string( fp, header->unityRevision, sizeof( header->unityRevision ) );
	header->minimumStreamedBytes = _read_int( fp );
	header->headerSize = _read_int( fp );
	header->numberOfLevelsToDownload = _read_int( fp );
	header->numberOfLevels = _read_int( fp );
	assert( header->numberOfLevelsToDownload == header->numberOfLevels || header->numberOfLevelsToDownload == 1 );
	if ( header->numberOfLevels > 0 ) {
		int i;
		header->levelByteEnd = ( LevelByteEnd_t* )malloc( sizeof( LevelByteEnd_t ) * header->numberOfLevelsToDownload );
		for ( i = 0; i < header->numberOfLevels; ++i ) {
			header->levelByteEnd[ i ].start = _read_int( fp );
			header->levelByteEnd[ i ].end = _read_int( fp );
		}
	}
	if ( header->streamVersion >= 2 ) {
		header->completeFileSize = _read_int( fp );
		assert( header->minimumStreamedBytes <= header->completeFileSize );
	}
	if ( header->streamVersion >= 3 ) {
		header->dataHeaderSize = _read_int( fp );
	}
	_read_byte( fp );
}

static void AB_WriteHeaderToFile( AssetBundleHeader_t* header, CSzFile* fp ) {
	_write_string( fp, header->assetType );
	_write_int( fp, header->streamVersion );
	_write_string( fp, header->unityVersion );
	_write_string( fp, header->unityRevision );
	_write_int( fp, header->minimumStreamedBytes );
	_write_int( fp, header->headerSize );
	_write_int( fp, header->numberOfLevelsToDownload );
	_write_int( fp, header->numberOfLevels );
	for ( int i = 0; i < header->numberOfLevels; ++i ) {
		_write_int( fp, header->levelByteEnd[ i ].start );
		_write_int( fp, header->levelByteEnd[ i ].end );
	}
	if ( header->streamVersion >= 2 ) {
		_write_int( fp, header->completeFileSize );
	}

	if ( header->streamVersion >= 3 ) {
		_write_int( fp, header->dataHeaderSize );
	}
	_write_byte( fp, 0 );
}

static int AB_IsValid( AssetBundleHeader_t* header ) {
	return strcmp( header->assetType, TAG_ASSETS_WEB ) == 0 ||
		strcmp( header->assetType, TAG_ASSETS_RAW ) == 0;
}

static void AB_SetCompressed( AssetBundleHeader_t* header, int compressed ) {
	strcpy( header->assetType, compressed ? TAG_ASSETS_WEB : TAG_ASSETS_RAW );
}

static int AB_IsCompressed( AssetBundleHeader_t* header ) {
	return strcmp( header->assetType, TAG_ASSETS_WEB ) == 0;
}

static int AB_GetUncompressSize( AssetBundleHeader_t* header ) {
	int size = header->headerSize;
	for ( int i = 0; i < header->numberOfLevels; ++i ) {
		size += header->levelByteEnd[ i ].end;
	}
	return size;
}

static SRes Decode2( CLzmaDec *state, ISeqOutStream *outStream, ISeqInStream *inStream,
	UInt64 unpackSize ) {
	int thereIsSize = ( unpackSize != ( UInt64 )( Int64 )-1 );
	Byte inBuf[IN_BUF_SIZE];
	Byte outBuf[OUT_BUF_SIZE];
	size_t inPos = 0, inSize = 0, outPos = 0;
	LzmaDec_Init( state );
	for ( ;;) {
		if ( inPos == inSize ) {
			inSize = IN_BUF_SIZE;
			RINOK( inStream->Read( inStream, inBuf, &inSize ) );
			inPos = 0;
		}
		{
			SRes res;
			SizeT inProcessed = inSize - inPos;
			SizeT outProcessed = OUT_BUF_SIZE - outPos;
			ELzmaFinishMode finishMode = LZMA_FINISH_ANY;
			ELzmaStatus status;
			if ( thereIsSize && outProcessed > unpackSize ) {
				outProcessed = ( SizeT )unpackSize;
				finishMode = LZMA_FINISH_END;
			}
			res = LzmaDec_DecodeToBuf( state, outBuf + outPos, &outProcessed,
				inBuf + inPos, &inProcessed, finishMode, &status );
			inPos += inProcessed;
			outPos += outProcessed;
			unpackSize -= outProcessed;
			if ( outStream ) {
				if ( outStream->Write( outStream, outBuf, outPos ) != outPos ) {
					return SZ_ERROR_WRITE;
				}
			}
			outPos = 0;
			if ( res != SZ_OK || ( thereIsSize && unpackSize == 0 ) ) {
				return res;
			}
			if ( inProcessed == 0 && outProcessed == 0 ) {
				if ( thereIsSize || status != LZMA_STATUS_FINISHED_WITH_MARK ) {
					return SZ_ERROR_DATA;
				}
				return res;
			}
		}
	}
}

static SRes Decode( ISeqOutStream *outStream, ISeqInStream *inStream ) {
	UInt64 unpackSize;
	int i;
	SRes res = 0;
	CLzmaDec state;
	/* header: 5 bytes of LZMA properties and 8 bytes of uncompressed size */
	unsigned char header[LZMA_PROPS_SIZE + 8];
	/* Read and parse header */
	RINOK( SeqInStream_Read( inStream, header, sizeof( header ) ) );
	unpackSize = 0;
	for ( i = 0; i < 8; i++ ) {
		unpackSize += ( UInt64 )header[LZMA_PROPS_SIZE + i] << ( i * 8 );
	}
	LzmaDec_Construct( &state );
	RINOK( LzmaDec_Allocate( &state, header, LZMA_PROPS_SIZE, &g_Alloc ) );
	res = Decode2( &state, outStream, inStream, unpackSize );
	LzmaDec_Free( &state, &g_Alloc );
	return res;
}

static const char* GetSResString( SRes res ) {
	const char* sret = "??";
	static const char* s[] = {
		"SZ_OK",
		"SZ_ERROR_DATA",
		"SZ_ERROR_MEM",
		"SZ_ERROR_CRC",
		"SZ_ERROR_UNSUPPORTED",
		"SZ_ERROR_PARAM",
		"SZ_ERROR_INPUT_EOF",
		"SZ_ERROR_OUTPUT_EOF",
		"SZ_ERROR_READ",
		"SZ_ERROR_WRITE",
		"SZ_ERROR_PROGRESS",
		"SZ_ERROR_FAIL",
		"SZ_ERROR_THREAD",
		"??",
		"??",
		"??",
		"SZ_ERROR_ARCHIVE",
		"SZ_ERROR_NO_ARCHIVE",
	};
	if ( res >= 0 && res < ( ( sizeof( s ) / sizeof( s[0] ) ) ) ) {
		sret = s[res];
	}
	return sret;
}

static void _mwrite_int32( char** p, Int32 value ) {
	Byte* bytes = ( Byte* )&value;
	Int32 _value = ( bytes[ 0 ] << 24 ) | ( bytes[ 1 ] << 16 ) | ( bytes[ 2 ] << 8 ) | bytes[ 3 ];
	memcpy( *p, &_value, sizeof( Int32 ) );
	*p += sizeof( Int32 );
}

#if KEEP_COMPRESSED_BUNDLE_HEADER
static void _mwrite_int16( char** p, Int16 value ) {
	Byte* bytes = ( Byte* )&value;
	Int16 _value = ( bytes[ 0 ] << 8 ) | bytes[ 1 ];
	memcpy( *p, &_value, sizeof( value ) );
	*p += sizeof( value );
}
#else
static void _write_int16( CSzFile* fp, Int16 value ) {
	Byte* bytes = ( Byte* )&value;
	size_t size = sizeof( Int16 );
	Int16 _value = ( bytes[ 0 ] << 8 ) | bytes[ 1 ];
	File_Write( fp, &_value, &size );
}
#endif

static size_t bwrite_bin( CSzFile* fp, const void* array, size_t arrayBytes ) {
	size_t size = arrayBytes;
	File_Write( fp, array, &size );
	return size == arrayBytes ? 1 : 0;
}

static size_t bread_bin( CSzFile* fp, void* array, size_t arrayBytes ) {
	size_t size = arrayBytes;
	File_Read( fp, array, &size );
	return size;
}

static int _copy_streaming( CSzFile* outFp, CSzFile* inpFp, int inSize ) {
	int osize = 0;
	char buf[ LZ4_BLOCK_BYTES ];
	for ( ; inSize > 0; ) {
		size_t readBytes = min( inSize, sizeof( buf ) );
		File_Read( inpFp, buf, &readBytes );
		if ( readBytes > 0 ) {
			inSize -= ( int )readBytes;
			File_Write( outFp, buf, &readBytes );
			osize += ( int )readBytes;
		} else {
			break;
		}
	}
	return osize;
}

static int _lz4_decompress_streaming( CSzFile* outFp, CSzFile* inpFp, int inSize ) {
	LZ4_streamDecode_t lz4StreamDecode_body;
	LZ4_streamDecode_t* lz4StreamDecode = &lz4StreamDecode_body;
	char decBuf[ 2 ][ LZ4_BLOCK_BYTES ];
	int decBufIndex = 0;
	int total = 0;
	LZ4_setStreamDecode( lz4StreamDecode, NULL, 0 );
	for ( ; inSize > 0; ) {
		char cmpBuf[ LZ4_COMPRESSBOUND( LZ4_BLOCK_BYTES ) ];
		int cmpBytes = min( inSize, sizeof( cmpBuf ) );
		int readBytes;
		inSize -= cmpBytes;
		readBytes = ( int )bread_bin( inpFp, cmpBuf, cmpBytes );
		if ( readBytes != cmpBytes ) {
			break;
		}
		{
			char* const decPtr = decBuf[ decBufIndex ];
			const int decBytes = LZ4_decompress_safe_continue(
				lz4StreamDecode, cmpBuf, decPtr, cmpBytes, LZ4_BLOCK_BYTES );
			if ( decBytes <= 0 ) {
				break;
			}
			total += decBytes;
			bwrite_bin( outFp, decPtr, ( size_t )decBytes );
		}
		decBufIndex = ( decBufIndex + 1 ) % 2;
	}
	return total;
}

static Int32 RequireDiskSpace( Int32 size, void* context ) {
	if ( _Hook != NULL ) {
		return _Hook( size, context );
	} else {
		return 1;
	}
}

static int _AB_Decompress_UnityFS( CFileSeqInStream* inStream, const char* inputFilePath, const char* outputFilePath, int* outFileSize, void* context ) {
	int result = ABDEC_ERROR_OK;
	char _SmallChunk1[256];
	char _SmallChunk2[512];
	int chunkMemType = 0;
	char* chunkInfoBytes = NULL;
	size_t chunkInfoSize = 0;
	int chunkInfoCompressType = 0;
	int blockCount = 0;
	UInt64 fileSize;
	int outUncompressedFileSize = 0;
	int finalOutFileSize = 0;
	Int64 outFilePadding = 0;
	ChunkBasedBundleHeader_t header;
	memset( &header, 0, sizeof( header ) );
	AB_ReadChunkBasedBundleHeader( &header, &inStream->file );
	for ( ; ; ) {
		if ( strcmp( header.assetType, TAG_ASSETS_FS ) != 0 ) {
			result = ABDEC_ERROR_INVALID_BUNDLE;
			break;
		}
		File_GetLength( &inStream->file, &fileSize );
		if ( ( header.flag & 0x3f ) == 0 ) {
			result = ABDEC_ERROR_OK_BYPASS;
			if ( outFileSize != NULL ) {
				*outFileSize = ( int )fileSize;
			}
		} else {
			CSzFile* fp = &inStream->file;
			/*alloc memory to store chunk info*/
			{
				chunkInfoSize = header.chunkInfoCompressedSize;
				if ( chunkInfoSize <= sizeof( _SmallChunk1 ) ) {
					chunkInfoBytes = _SmallChunk1;
					chunkMemType = 1;
				} else {
					chunkInfoBytes = ( char* )malloc( chunkInfoSize );
				}
			}
			/*jump to EOF*/
			{
				if ( ( header.flag & 0x80 ) != 0 ) {
					Int64 curPos = 0;
					Int64 tempPos = fileSize - chunkInfoSize;
					File_Seek( fp, &curPos, SZ_SEEK_CUR );
					File_Seek( fp, &tempPos, SZ_SEEK_SET );
					File_Read( fp, chunkInfoBytes, &chunkInfoSize );
					File_Seek( fp, &curPos, SZ_SEEK_CUR );
				} else {
					File_Read( fp, chunkInfoBytes, &chunkInfoSize );
				}
			}
			/*decompress chunk info data*/
			{
				chunkInfoCompressType = header.flag & 0x3f;
				if ( chunkInfoCompressType != 0 ) {
					int ok = 0;
					char* uncompressData = NULL;
					int _chunkMemType2 = 0;
					if ( header.chunkInfoUncompressedSize <= sizeof( _SmallChunk2 ) ) {
						uncompressData = _SmallChunk2;
						_chunkMemType2 = 2;
					} else {
						uncompressData = ( char* )malloc( header.chunkInfoUncompressedSize );
					}
					if ( chunkInfoCompressType == 1 ) {
						ELzmaStatus lzmaStatus;
						SRes res;
						SizeT destLen, srcLen;
						unsigned char props[ LZMA_PROPS_SIZE ];
						char* p = chunkInfoBytes;
						memcpy( props, p, sizeof( props ) );
						p += sizeof( props );
						destLen = header.chunkInfoUncompressedSize;
						srcLen = header.chunkInfoCompressedSize;
						res = LzmaDecode( ( Byte* )uncompressData, &destLen, ( Byte* )p, &srcLen, props, sizeof( props ), LZMA_FINISH_ANY, &lzmaStatus, &g_Alloc );
						ok = res == SZ_OK && lzmaStatus != LZMA_STATUS_NOT_FINISHED;
					} else {
						int count = LZ4_decompress_safe( chunkInfoBytes, uncompressData, ( int )chunkInfoSize, header.chunkInfoUncompressedSize );
						ok = count == header.chunkInfoUncompressedSize;
					}
					if ( chunkMemType == 0 ) {
						free( chunkInfoBytes );
					}
					if ( !ok ) {
						result = ABDEC_ERROR_DEC_FAILED;
						break;
					}
					chunkInfoBytes = uncompressData;
					chunkInfoSize = header.chunkInfoUncompressedSize;
					chunkMemType = _chunkMemType2;
				}
			}
			/*calculate output uncompressed file size*/
			{
				int i;
				const char* blockInfoPtr;
				char guid[16];
				int chunkFlags = 0;
				blockInfoPtr = chunkInfoBytes;
				memcpy( guid, blockInfoPtr, sizeof( guid ) );
				blockInfoPtr += sizeof( guid );
				blockCount = _mread_int( &blockInfoPtr );
				for ( i = 0; i < blockCount; ++i ) {
					int compressSize, uncompressSize;
					uncompressSize = _mread_int( &blockInfoPtr );
					compressSize = _mread_int( &blockInfoPtr );
					chunkFlags |= _mread_int16( &blockInfoPtr );
					outUncompressedFileSize += uncompressSize;
				}
				if ( ( chunkFlags & 0x3f ) == 0 ) {
					result = ABDEC_ERROR_OK_BYPASS;
					if ( outFileSize != NULL ) {
						*outFileSize = ( int )fileSize;
					}
					break;
				}
				{
					int requireSize = ( int )( header.headerSize + chunkInfoSize + outUncompressedFileSize );
					finalOutFileSize = requireSize;
					header.bundleSize = finalOutFileSize;
					if ( 0 == RequireDiskSpace( requireSize, context ) ) {
						result = ABDEC_ERROR_DISKSPACE_FAIL;
						break;
					}
				}
			}
			CFileOutStream outStream;
			{
				FileOutStream_CreateVTable( &outStream );
				File_Construct( &outStream.file );
				if ( OutFile_Open( &outStream.file, outputFilePath ) != 0 ) {
					printf( "can't open out file: %s", outputFilePath );
					result = ABDEC_ERROR_OUTPUT;
					break;
				}
			}
			CSzFile* ofp = &outStream.file;
			/*erase EOF bits*/
			header.flag &= ~0x80;
			/*modify & store chunk info*/
			if ( chunkInfoCompressType != 0 ) {
				int i;
				char* blockInfoPtr;
#if KEEP_COMPRESSED_BUNDLE_HEADER
				int cloned_UseHeap = 0;
				char _SmallChunk3[512];
				char* cloned_chunkInfoBytes = NULL;
				if ( chunkInfoSize <= sizeof( _SmallChunk3 ) ) {
					cloned_chunkInfoBytes = _SmallChunk3;
				} else {
					cloned_UseHeap = 1;
					cloned_chunkInfoBytes = ( char* )malloc( chunkInfoSize );
				}
				memcpy( cloned_chunkInfoBytes, chunkInfoBytes, chunkInfoSize );
				blockInfoPtr = cloned_chunkInfoBytes + 20;
				for ( i = 0; i < blockCount; ++i ) {
					int uncompressSize, compressSize, flag;
					uncompressSize = _mread_int( ( const char** )&blockInfoPtr );
					compressSize = _mread_int( ( const char** )&blockInfoPtr );
					flag = _mread_int16( ( const char** )&blockInfoPtr );
					blockInfoPtr -= 6;
					_mwrite_int32( &blockInfoPtr, uncompressSize );
					_mwrite_int16( &blockInfoPtr, ( Int16 )( flag & ~0x3f ) );
				}
				{
					char* compressedBuf = NULL;
					int compressedSize = 0;
					int useHeap = 0;
					int dstSize = LZ4_compressBound( ( int )chunkInfoSize );
					if ( dstSize <= sizeof( _SmallChunk1 ) && chunkMemType != 1 ) {
						compressedBuf = _SmallChunk1;
						dstSize = sizeof( _SmallChunk1 );
					} else if ( dstSize <= sizeof( _SmallChunk2 ) && chunkMemType != 2 ) {
						compressedBuf = _SmallChunk2;
						dstSize = sizeof( _SmallChunk2 );
					} else {
						useHeap = 1;
						compressedBuf = ( char* )malloc( ( size_t )dstSize );
					}
					compressedSize = LZ4_compress_default( cloned_chunkInfoBytes, compressedBuf, ( int )chunkInfoSize, header.chunkInfoCompressedSize );
					if ( compressedSize > 0 ) {
						size_t _size = ( size_t )compressedSize;
						/*use lz4*/
						header.flag &= ~0x3f;
						header.flag |= 3;
						assert( header.chunkInfoUncompressedSize == chunkInfoSize );
						header.chunkInfoCompressedSize = compressedSize;
						finalOutFileSize = header.headerSize + compressedSize + outUncompressedFileSize;
						header.bundleSize = finalOutFileSize;
						AB_WriteChunkBasedBundleHeader( &header, ofp );
						File_Write( ofp, compressedBuf, &_size );
					}
					if ( useHeap ) {
						free( compressedBuf );
					}
					if ( cloned_UseHeap ) {
						free( cloned_chunkInfoBytes );
					}
					if ( compressedSize <= 0 ) {
						result = ABDEC_ERROR_DEC_FAILED;
						break;
					}
				}
#else
				{
					/*remove compress type bits*/
					header.flag &= ~0x3f;
					header.chunkInfoCompressedSize = header.chunkInfoUncompressedSize;
					AB_WriteChunkBasedBundleHeader( &header, ofp );
				}
				Int64 curPos = 0, postPos = 0;
				File_Seek( ofp, &curPos, SZ_SEEK_CUR );
				/*write all original chunkInfoBytes*/
				File_Write( ofp, chunkInfoBytes, &chunkInfoSize );
				File_Seek( ofp, &postPos, SZ_SEEK_CUR );
				/*jump to start*/
				curPos += 20; /*guid + blockCount*/
				blockInfoPtr = chunkInfoBytes + 20;
				File_Seek( ofp, &curPos, SZ_SEEK_SET );
				for ( i = 0; i < blockCount; ++i ) {
					int uncompressSize, compressSize, flag;
					uncompressSize = _mread_int( ( const char** )&blockInfoPtr );
					compressSize = _mread_int( ( const char** )&blockInfoPtr );
					flag = _mread_int16( ( const char** )&blockInfoPtr );
					_write_int( ofp, uncompressSize );
					_write_int( ofp, uncompressSize );
					_write_int16( ofp, ( Int16 )( flag & ~0x3f ) );
				}
				File_Seek( ofp, &postPos, SZ_SEEK_SET );
#endif
			} else {
				AB_WriteChunkBasedBundleHeader( &header, ofp );
				assert( ( header.flag & 0x3f ) == 0 );
				assert( header.chunkInfoCompressedSize == header.chunkInfoUncompressedSize );
				/*write original chunk info directly for uncompressed data*/
				File_Write( ofp, chunkInfoBytes, &chunkInfoSize );
			}
			/*decompress main data*/
			if ( chunkInfoBytes != NULL ) {
				int blockCount;
				int i, uncompressSize, compressSize, flag;
				char* blockInfoPtr = chunkInfoBytes;
				SRes ret;
				blockInfoPtr += 16; /*16 bytes GUID here*/
				blockCount = _mread_int( &blockInfoPtr );
				for ( i = 0; i < blockCount; ++i ) {
					int ok = 0;
					char* p_uncompressSize = blockInfoPtr;
					uncompressSize = _mread_int( &blockInfoPtr );
					compressSize = _mread_int( &blockInfoPtr );
					flag = _mread_int16( &blockInfoPtr ) & 0x3f;
					switch ( flag ) {
					case 0:
						ok = _copy_streaming( ofp, fp, uncompressSize ) == uncompressSize;
						break;
					case 1: {
							CLzmaDec state;
							unsigned char header[ LZMA_PROPS_SIZE ];
							Int64 outBegin, outEnd;
							outBegin = 0;
							outEnd = 0;
							RINOK( SeqInStream_Read( &inStream->s, header, sizeof( header ) ) );
							LzmaDec_Construct( &state );
							RINOK( LzmaDec_Allocate( &state, header, LZMA_PROPS_SIZE, &g_Alloc ) );
							outBegin = 0;
							File_Seek( ofp, &outBegin, SZ_SEEK_CUR );
							ret = Decode2( &state, &outStream.s, &inStream->s, -1 );
							outEnd = 0;
							File_Seek( ofp, &outEnd, SZ_SEEK_CUR );
							LzmaDec_Free( &state, &g_Alloc );
							ok = ret == SZ_OK;
							if ( ok ) {
								Int64 realUncompressSize = outEnd - outBegin;
								outFilePadding = ( Int64 )( uncompressSize - ( outEnd - outBegin ) );
								if ( outFilePadding > 0 ) {
									/*fix block's uncompress size*/
									_mwrite_int32( &p_uncompressSize, ( Int32 )realUncompressSize );
								}
							}
						}
						break;
					case 2:
					case 3:
						ok = _lz4_decompress_streaming( ofp, fp, compressSize ) == uncompressSize;
						break;
					}
					if ( !ok ) {
						result = ABDEC_ERROR_DEC_FAILED;
						break;
					}
				}
			}
			/*check input file*/
			{
				Int64 pos = 0;
				File_Seek( fp, &pos, SZ_SEEK_CUR );
				assert( pos == fileSize && inputFilePath );
			}
			/*check output file*/
			{
#if PADDING_UNCOMPRESSED_FILE_SIZE
				/*check uncompress data size*/
				/*we need pad file size if uncompress size is smaller than size declared in file header*/
				char dummy[ OUT_BUF_SIZE ];
				Int64 clearnum;
				size_t outPadding = ( size_t )outFilePadding;
				clearnum = min( sizeof( dummy ), outPadding );
				memset( dummy, 0, ( size_t )clearnum );
				while ( outPadding > 0 ) {
					size_t n = min( outPadding, sizeof( dummy ) );
					outPadding -= n;
					File_Write( ofp, dummy, &n );
				}
#else
				finalOutFileSize -= ( int )outFilePadding;
				{
					/*fix bundle size*/
					Int64 pos = 0;
					Int64 cur = 0;
					header.bundleSize -= outFilePadding;
					File_Seek( ofp, &cur, SZ_SEEK_CUR );
					File_Seek( ofp, &pos, SZ_SEEK_SET );
					AB_WriteChunkBasedBundleHeader( &header, ofp );
					/*fix block info*/
					assert( ( header.flag & 0x3f ) == 0 );
					assert( header.chunkInfoCompressedSize == header.chunkInfoUncompressedSize );
					/*write original chunk info directly for uncompressed data*/
					File_Write( ofp, chunkInfoBytes, &chunkInfoSize );
					/*restore stream position*/
					File_Seek( ofp, &cur, SZ_SEEK_SET );
				}
#endif
				Int64 pos = 0;
				File_Seek( ofp, &pos, SZ_SEEK_CUR );
				if ( pos != finalOutFileSize ) {
					result = ABDEC_ERROR_DEC_FAILED;
				}
				if ( outFileSize != NULL ) {
					*outFileSize = finalOutFileSize;
				}
			}
			File_Close( &outStream.file );
			if ( result < 0 ) {
				/*delete error output file if errors happen*/
				if ( 0 != remove( outputFilePath ) ) {
					result = ABDEC_ERROR_FATAL;
				}
			}
		}
		break;
	}					
	if ( chunkMemType == 0 ) {
		free( chunkInfoBytes );
	}
	return result;
}

static int _AB_Decompress_UnityWeb( CFileSeqInStream* inStream, const char* inputFilePath, const char* outputFilePath, int* outFileSize, void* context ) {
	int result = ABDEC_ERROR_OK;
	AssetBundleHeader_t* header;
	for ( ; ; ) {
		AssetBundleHeader_t _header;
		memset( &_header, 0, sizeof( _header ) );
		header = &_header;
		AB_ReadHeaderFromFile( header, &inStream->file );
		if ( AB_IsValid( header ) ) {
			if ( AB_IsCompressed( header ) ) {
				int ret;
				Int64 pos = 0;
				int fileSize = AB_GetUncompressSize( header );
				if ( 0 == RequireDiskSpace( fileSize, context ) ) {
					result = ABDEC_ERROR_DISKSPACE_FAIL;
					break;
				}
				int bundleSize = 0;
				CFileOutStream outStream;
				FileOutStream_CreateVTable( &outStream );
				File_Construct( &outStream.file );
				if ( OutFile_Open( &outStream.file, outputFilePath ) != 0 ) {
					printf( "can't open out file: %s", outputFilePath );
					result = ABDEC_ERROR_OUTPUT;
					break;
				}
				AB_SetCompressed( header, 0 );
				header->minimumStreamedBytes = 0;
				header->completeFileSize = 0;
				AB_WriteHeaderToFile( header, &outStream.file );
				ret = Decode( &outStream.s, &inStream->s );
				File_Seek( &outStream.file, &pos, SZ_SEEK_CUR );
				bundleSize = ( int )pos;
				pos = 0;
				File_Seek( &outStream.file, &pos, SZ_SEEK_SET );
				header->minimumStreamedBytes = bundleSize;
				header->completeFileSize = bundleSize;
				AB_WriteHeaderToFile( header, &outStream.file );
				File_Close( &outStream.file );
				if ( ret != SZ_OK ) {
					const char* e = GetSResString( ret );
					fprintf( stderr, "LZMA Decode failed: %s, error = %s, %d\n", inputFilePath, e, ret );
				}
				result = ( ret == SZ_OK && fileSize == bundleSize ) ?
					ABDEC_ERROR_OK : ABDEC_ERROR_DEC_FAILED;
				if ( result == ABDEC_ERROR_OK && outFileSize != NULL ) {
					*outFileSize = fileSize;
				} else {
					if ( 0 != remove( outputFilePath ) ) {
						result = ABDEC_ERROR_FATAL;
					}
				}
			} else {
				UInt64 len = 0;
				File_GetLength( &inStream->file, &len );
				result = ABDEC_ERROR_OK_BYPASS;
				if ( outFileSize != NULL ) {
					*outFileSize = ( int )len;
				}
			}
		} else {
			result = ABDEC_ERROR_INVALID_BUNDLE;
		}
		AB_DestroyHeaderData( header );
		break;
	}
	return result;
}

ABD_API Int32 AB_SetupRequireDiskSpaceHook( RequireDiskSpaceHook hook ) {
	_Hook = hook;
	return 1;
}

ABD_API int AB_Decompress( const char* inputFilePath, const char* outputFilePath, int* outFileSize, void* context ) {
	CFileSeqInStream inStream;
	int result = ABDEC_ERROR_OK;
	int format = 0;
	FileSeqInStream_CreateVTable( &inStream );
	File_Construct( &inStream.file );
	if ( InFile_Open( &inStream.file, inputFilePath ) != 0 ) {
		printf( "can't open input file: %s", inputFilePath );
		return ABDEC_ERROR_INPUT;
	}
	format = AB_ReadFormatFromFile( &inStream.file );
	if ( format >= 6 ) {
		result = _AB_Decompress_UnityFS( &inStream, inputFilePath, outputFilePath, outFileSize, context );
	} else {
		result = _AB_Decompress_UnityWeb( &inStream, inputFilePath, outputFilePath, outFileSize, context );
	}
	File_Close( &inStream.file );
	return result;
}

/*EOF*/
