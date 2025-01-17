//=============================================================================
// ImageEncoder.cpp: implementation of the ImageEncoder class.
//		Replaces Paintlib dependency for writing image files from bmp.
//=============================================================================
#include <windows.h>

#include <stdlib.h>
#include <string>
#include <iostream>

#include <atlstr.h>
#include <atlimage.h>

// This class replaces the following Paintlib dependencies, which are no longer maintained
// and do not support 64-bit versions
/*
#include <paintlib/plstdpch.h>
#include <paintlib/planydec.h>
#include <paintlib/plwinbmp.h>
#include <paintlib/plbmpenc.h>
#include <paintlib/pljpegenc.h>
#include <paintlib/plpngenc.h>
#include <paintlib/pltiffenc.h>
*/
#include "ImageEncoder.h"


using namespace std;

//=============================================================================

ImageEncoder::ImageEncoder() {

}

ImageEncoder::~ImageEncoder() {

}

// Export BMP from a bitmap handle
void ImageEncoder::saveBmp(char* fileName, HBITMAP hbm, HDC hdc) {
	PBITMAPINFO pbi = CreateBitmapInfoStruct(hbm);
	CreateBMPFile((LPTSTR)fileName, pbi, hbm, hdc);
}

// Export bitmap handle to JPEG
void ImageEncoder::saveJpegFromBmp(char* fileName, HBITMAP hbm) {
	checkExtension(fileName, ".jpg");
	saveImage(fileName, hbm);
}

// Export bitmap handle to PNG
void ImageEncoder::savePngFromBmp(char* fileName, HBITMAP hbm) {
	checkExtension(fileName, ".png");
	saveImage(fileName, hbm);
}

// Export bitmap handle to TIFF
void ImageEncoder::saveTiffFromBmp(char* fileName, HBITMAP hbm) {
	checkExtension(fileName, ".tif");
	saveImage(fileName, hbm);
}

void ImageEncoder::checkExtension(char* fileName, const char* ext) {
	int n = strlen(fileName);
	if (n < 4 || std::strncmp(fileName + n - 4, ext, 4) != 0 ) {
		strcat_s(fileName, strlen(fileName) + 5, ext);
	}
}

void ImageEncoder::saveImage(const char* fileName, HBITMAP hbm) {
	CImage image;
	image.Attach(hbm);
	image.Save(fileName);
}





// ======================================================================
// Reference: https://docs.microsoft.com/en-us/windows/win32/gdi/storing-an-image?redirectedfrom=MSDN

PBITMAPINFO ImageEncoder::CreateBitmapInfoStruct(HBITMAP hBmp)
{
	BITMAP bmp;
	PBITMAPINFO pbmi;
	WORD    cClrBits;

	// Retrieve the bitmap color format, width, and height.  
	GetObject(hBmp, sizeof(BITMAP), (LPSTR)&bmp);

	// Convert the color format to a count of bits.  
	cClrBits = (WORD)(bmp.bmPlanes * bmp.bmBitsPixel);
	if (cClrBits == 1)
		cClrBits = 1;
	else if (cClrBits <= 4)
		cClrBits = 4;
	else if (cClrBits <= 8)
		cClrBits = 8;
	else if (cClrBits <= 16)
		cClrBits = 16;
	else if (cClrBits <= 24)
		cClrBits = 24;
	else cClrBits = 32;

	// Allocate memory for the BITMAPINFO structure. (This structure  
	// contains a BITMAPINFOHEADER structure and an array of RGBQUAD  
	// data structures.)  

	if (cClrBits < 24)
		pbmi = (PBITMAPINFO)LocalAlloc(LPTR,
			sizeof(BITMAPINFOHEADER) +
			sizeof(RGBQUAD) * (1 << cClrBits));

	// There is no RGBQUAD array for these formats: 24-bit-per-pixel or 32-bit-per-pixel 

	else
		pbmi = (PBITMAPINFO)LocalAlloc(LPTR,
			sizeof(BITMAPINFOHEADER));

	// Initialize the fields in the BITMAPINFO structure.  

	pbmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	pbmi->bmiHeader.biWidth = bmp.bmWidth;
	pbmi->bmiHeader.biHeight = bmp.bmHeight;
	pbmi->bmiHeader.biPlanes = bmp.bmPlanes;
	pbmi->bmiHeader.biBitCount = bmp.bmBitsPixel;
	if (cClrBits < 24)
		pbmi->bmiHeader.biClrUsed = (1 << cClrBits);

	// If the bitmap is not compressed, set the BI_RGB flag.  
	pbmi->bmiHeader.biCompression = BI_RGB;

	// Compute the number of bytes in the array of color  
	// indices and store the result in biSizeImage.  
	// The width must be DWORD aligned unless the bitmap is RLE 
	// compressed. 
	pbmi->bmiHeader.biSizeImage = ((pbmi->bmiHeader.biWidth * cClrBits + 31) & ~31) / 8
		* pbmi->bmiHeader.biHeight;
	// Set biClrImportant to 0, indicating that all of the  
	// device colors are important.  
	pbmi->bmiHeader.biClrImportant = 0;
	return pbmi;
}

void ImageEncoder::CreateBMPFile(LPTSTR pszFile, PBITMAPINFO pbi, HBITMAP hBMP, HDC hDC)
{
	HANDLE hf;                 // file handle  
	BITMAPFILEHEADER hdr;       // bitmap file-header  
	PBITMAPINFOHEADER pbih;     // bitmap info-header  
	LPBYTE lpBits;              // memory pointer  
	DWORD dwTotal;              // total count of bytes  
	DWORD cb;                   // incremental count of bytes  
	BYTE *hp;                   // byte pointer  
	DWORD dwTmp;

	pbih = (PBITMAPINFOHEADER)pbi;
	lpBits = (LPBYTE)GlobalAlloc(GMEM_FIXED, pbih->biSizeImage);


	// Retrieve the color table (RGBQUAD array) and the bits  
	// (array of palette indices) from the DIB.  
	GetDIBits(
		hDC,
		hBMP,
		0,
		(WORD)pbih->biHeight,
		lpBits,
		pbi,
		DIB_RGB_COLORS);

	// Create the .BMP file.  
	hf = CreateFile(pszFile,
		GENERIC_READ | GENERIC_WRITE,
		(DWORD)0,
		NULL,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		(HANDLE)NULL);

	hdr.bfType = 0x4d42;        // 0x42 = "B" 0x4d = "M"  
	// Compute the size of the entire file.  
	hdr.bfSize = (DWORD)(sizeof(BITMAPFILEHEADER) +
		pbih->biSize + pbih->biClrUsed
		* sizeof(RGBQUAD) + pbih->biSizeImage);
	hdr.bfReserved1 = 0;
	hdr.bfReserved2 = 0;

	// Compute the offset to the array of color indices.  
	hdr.bfOffBits = (DWORD) sizeof(BITMAPFILEHEADER) +
		pbih->biSize + pbih->biClrUsed
		* sizeof(RGBQUAD);

	// Copy the BITMAPFILEHEADER into the .BMP file.  
	WriteFile(hf, (LPVOID)&hdr, sizeof(BITMAPFILEHEADER),
		(LPDWORD)&dwTmp, NULL);

	// Copy the BITMAPINFOHEADER and RGBQUAD array into the file.  
	WriteFile(hf, (LPVOID)pbih, sizeof(BITMAPINFOHEADER)
		+ pbih->biClrUsed * sizeof(RGBQUAD),
		(LPDWORD)&dwTmp, (NULL));

	// Copy the array of color indices into the .BMP file.  
	dwTotal = cb = pbih->biSizeImage;
	hp = lpBits;
	WriteFile(hf, (LPSTR)hp, (int)cb, (LPDWORD)&dwTmp, NULL);

	// Close the .BMP file.  
	CloseHandle(hf);

	// Free memory.  
	GlobalFree((HGLOBAL)lpBits);
}