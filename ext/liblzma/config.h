/* config.h for building liblzma with MSVC on Windows */

/* Standard headers available on MSVC */
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDBOOL_H 1

/* Size of size_t */
#ifdef _WIN64
#define SIZEOF_SIZE_T 8
#else
#define SIZEOF_SIZE_T 4
#endif

/* Threading: use Windows Vista+ threads */
#define MYTHREAD_VISTA 1

/* We only need decoders for reading archives */
#define HAVE_DECODERS 1

/* Decoders needed for 7zip support */
#define HAVE_DECODER_LZMA1 1
#define HAVE_DECODER_LZMA2 1
#define HAVE_DECODER_DELTA 1
#define HAVE_DECODER_X86 1

/* Integrity checks */
#define HAVE_CHECK_CRC32 1
#define HAVE_CHECK_CRC64 1

/* Assume little-endian on Windows */
#define TUKLIB_FAST_UNALIGNED_ACCESS 1

/* CRC tables */
#ifdef _MSC_VER
#pragma warning(disable: 4028 4113 4133 4244 4267 4996)
#endif
