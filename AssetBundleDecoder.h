/*
Unity AssetBundle decompressor

support: unity4.7+, unity5.3+ lzma format
platform: x86, x64, ios, android
warning: decompress chunk based bundle not test

Copyright (c) 2014 - 2018, lujian
All rights reserved.
email: lujian_niewa@sina.com, 345005607@qq.com
*/
#ifndef __ASSETBUNDLE_UTIL_H__
#define __ASSETBUNDLE_UTIL_H__

#include <inttypes.h>

#if defined( BUILD_AS_DLL )
	#if defined( ABD_CORE ) || defined( ABD_LIB )
		#define ABD_API __declspec( dllexport )
	#else
		#define ABD_API __declspec( dllimport )
	#endif
#else
	#define ABD_API extern
#endif

/*AB_Decompress return values*/
#define ABDEC_ERROR_DISKSPACE_FAIL  -6
#define ABDEC_ERROR_FATAL			-5
#define ABDEC_ERROR_DEC_FAILED		-4
#define ABDEC_ERROR_OUTPUT			-3
#define ABDEC_ERROR_INVALID_BUNDLE	-2
#define ABDEC_ERROR_INPUT			-1
#define ABDEC_ERROR_OK				 0
#define ABDEC_ERROR_OK_BYPASS		 1

/*decompress assetbundle to uncompressed file*/
ABD_API Int32 AB_Decompress(
	const char* inputFilePath,
	const char* outputFilePath,
	Int32* outFileSize,
	void* context );

/*callback for disk space checking before write uncompressed data to disk*/
typedef Int32( *RequireDiskSpaceHook )( Int32 size, void* context );
ABD_API Int32 AB_SetupRequireDiskSpaceHook( RequireDiskSpaceHook hook );

#endif
/*EOF*/
