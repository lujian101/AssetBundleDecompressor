# Unity AssetBundle Decompressor

Fast AssetBundle decompressor implemented in C

Load compressed AssetBundle in unity is very slow, so you could use this library to convert all your assetbundles to uncompressed bundle.

features:
* support Unity4.6+, Unity5.3+
* platform: x86, x64, ios, android
* thread safe ( which means you can decompress files in threads simultaneously )

warning:
* chunk based format decompress has not widely tested
* library does nothing if input bundle is already uncompressed

