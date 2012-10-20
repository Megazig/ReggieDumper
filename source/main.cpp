/*
 *  Reggie Dumper
 *  Copyright (C) 2012 AerialX, megazig
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <gccore.h>
#include <wiiuse/wpad.h>

#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/stat.h>

#include <fat.h>

#include <string>
using std::string;

#include "wdvd.h"

#define PART_OFFSET			0x00040000

#define min(a,b) ((a)>(b) ? (b) : (a))
#define max(a,b) ((a)>(b) ? (a) : (b))

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

struct DiscNode;
static DiscNode* fst = NULL;
static u32 partition_info[24] ATTRIBUTE_ALIGN(32);

struct DiscNode
{
	u8 Type;
	u8 NameOffsetMSB;
	u16 NameOffset; //really a u24
	u32 DataOffset;
	u32 Size;
	u32 GetNameOffset() { return ((u32)NameOffsetMSB << 16) | NameOffset; }
	void SetNameOffset(u32 offset) { NameOffset = (u16)offset; NameOffsetMSB = offset >> 16; }
	const char* GetName() { return (const char*)(fst + fst->Size) + GetNameOffset(); }
} __attribute__((packed));

static DiscNode* RVL_FindNode(const char* name, DiscNode* root, bool recursive)
{
	const char* nametable = (const char*)(fst + fst->Size);
	int offset = root - fst;
	DiscNode* node = root;
	while ((void*)node < (void*)nametable) {
		if (!strcasecmp(nametable + node->GetNameOffset(), name))
			return node;

		if (recursive || node->Type == 0)
			node++;
		else
			node = root + node->Size - offset;
	}

	return NULL;
}

DiscNode* RVL_FindNode(const char* fstname)
{
	if (fstname[0] != '/')
		return RVL_FindNode(fstname, fst, true);

	char namebuffer[MAXPATHLEN];
	char* name = namebuffer;
	strcpy(name, fstname + 1);

	DiscNode* root = fst;

	while (root) {
		char* slash = strchr(name, '/');
		if (!slash)
			return RVL_FindNode(name, root + 1, false);

		*slash = '\0';
		root = RVL_FindNode(name, root + 1, false);
		name = slash + 1;
	}

	return NULL;
}

static u32 fstdata[0x40] ATTRIBUTE_ALIGN(32);
bool Launcher_ReadFST()
{
	memset(partition_info, 0, sizeof(partition_info));
	if (WDVD_LowUnencryptedRead(partition_info, 0x20, PART_OFFSET) || partition_info[0] == 0)
		return false;
	if (WDVD_LowUnencryptedRead(partition_info + 8, max(4, min(8, partition_info[0])) * 8, (u64)partition_info[1] << 2))
		return false;
	u32 i;
	for (i = 0; i < partition_info[0]; i++)
		if (partition_info[i * 2 + 8 + 1] == 0)
			break;
	if (i >= partition_info[0])
		return false;
	if (WDVD_LowOpenPartition((u64)partition_info[i * 2 + 8] << 2))
		return false;

	if (WDVD_LowRead(fstdata, 0x40, 0x420))
		return false;

	fstdata[2] <<= 2;
	fst = (DiscNode*)memalign(32, fstdata[2]);
	if (WDVD_LowRead(fst, fstdata[2], (u64)fstdata[1] << 2))
		return false;

	return true;
}

bool Launcher_DiscInserted()
{
	bool cover;
	if (!WDVD_VerifyCover(&cover))
		return cover;
	return false;
}

#define HOME_EXIT() { \
	WPAD_ScanPads(); \
	if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) { \
		if (*(vu32*)0x80001804 != 0x53545542) \
			SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0); \
		else \
			return 0; \
	} \
	VIDEO_WaitVSync(); \
}

string PathCombine(string path, string file)
{
	if (path.empty())
		return file;
	if (file.empty())
		return path;

	bool flag = true;
	while (flag) {
		flag = false;
		if (!strncmp(file.c_str(), "../", 3)) {
			string::size_type pos = path.find_last_of('/', path.size() - 1);
			if (pos != string::npos)
				path = path.substr(0, pos);

			file = file.substr(3);

			flag = true;
		} else if (!strncmp(file.c_str(), "./", 2)) {
			file = file.substr(2);

			flag = true;
		}
	}

	if (path[path.size() - 1] == '/' && file[0] == '/')
		return path + file.substr(1);

	if (path[path.size() - 1] == '/' || file[0] == '/')
		return path + file;

	return path + "/" + file;
}

bool DumpFolder(DiscNode* node, string path)
{
	if (!node)
		return false;

	if (!node->Type)
		return false;

	mkdir(path.c_str(), 0777);
	printf("\nDumping to %s...", path.c_str());

	for (DiscNode* file = node + 1; file < fst + node->Size; ) {
		string name = PathCombine(path, file->GetName());
		if (file->Type) {
			bool ret = DumpFolder(file, name);
			if (!ret)
				return false;
		} else {
			FILE* fd = fopen(name.c_str(), "wb");
			if (fd) {
				static u8 buffer[0x8000] ATTRIBUTE_ALIGN(32);
				memset(buffer, 0, 0x8000);
				u32 written = 0;
				u32 toRead = (file->Size > 0x8000) ? 0x8000 : file->Size;
				while (written < file->Size) {
					if (WDVD_LowRead(buffer, toRead, ((u64)file->DataOffset << 2) + written))
						return false;
					int towrite = MIN(sizeof(buffer), file->Size - written);
					fwrite(buffer, 1, towrite, fd);
					written += towrite;
					if ( file->Size - written < 0x8000 ) toRead = file->Size - written;
				}
				fclose(fd);
				printf(".");
			} else {
				return false;
			}
		}
		file = file->Type ? fst + file->Size : file + 1;
	}

	return true;
}

bool DumpFolder(const char* disc, string path)
{
	DiscNode* node = !strcmp(disc, "/") ? fst : RVL_FindNode(disc);
	return DumpFolder(node, path);
}

bool DumpMainDol(void)
{
	FILE *header_nfo = fopen("/reggie/header.bin", "wb");
	if (!header_nfo)
		return false;

	u32 written = fwrite((void*)0x80000000, 1, 0x20, header_nfo);
	if (written != 0x20) {
		printf("Expected %d got %d\n", 0x20, written);
		fclose(header_nfo);
		return false;
	}
	fclose(header_nfo);
	printf("Dumped header.bin\n");

	u8 * dol_nfo = (u8*)memalign(32, 0x100);
	if (!dol_nfo)
		return false;

	WDVD_LowRead(dol_nfo, 0x100, (u64)fstdata[0] << 2);
    u32 max = 0;
    for (u32 i = 0; i < 7; i++) {
        u32 offset = *(u32 *)(dol_nfo + (i * 4));
        u32 size = *(u32 *)(dol_nfo + (i * 4) + 0x90);
        if ((offset + size) > max) max = offset + size;
    }
    for (u32 i = 0; i < 11; i++) {
        u32 offset = *(u32 *)(dol_nfo + (i * 4) + 0x1c);
        u32 size = *(u32 *)(dol_nfo + (i * 4) + 0xac);
        if ((offset + size) > max) max = offset + size;
    }
	free(dol_nfo);

	void * dol = memalign(32, max);
	if (!dol)
		return false;

	if (WDVD_LowRead(dol, max, (u64)fstdata[0] << 2))
	{
		printf("Failed to read main.dol\n");
		free(dol);
		return false;
	}

	FILE *main_dol = fopen("/reggie/main.dol", "wb");
	if (!main_dol)
	{
		printf("Failed to open /reggie/main.dol for write\n");
		free(dol);
		return false;
	}

	bool ret = true;
	written = fwrite(dol, 1, max, main_dol);
	if (written != max)
	{
		printf("Expected %d got %d\n", max, written);
		ret = false;
	}
	else
	{
		printf("Dumped main.dol\n");
	}
	fclose(main_dol);
	free(dol);
	return ret;
}

int main(int argc, char **argv)
{
	VIDEO_Init();
	WPAD_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	CON_Init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE); VIDEO_Flush(); VIDEO_WaitVSync(); VIDEO_WaitVSync();

	printf("\x1b[2;0H");
	printf("Reggie's Dump v1.0\n\n");
	printf("Press Home at any time to exit.\n");

	printf("Looking for SD/USB... ");
	while (!fatInitDefault())
		HOME_EXIT();

	printf("Found!\n");

	WDVD_Init();

reinsert_disc:
	printf("Insert disc... ");

	WDVD_Reset();

	while (!Launcher_DiscInserted())
		HOME_EXIT();

	printf("Done.\n");

	printf("Checking disc...\n");

	if (WDVD_LowReadDiskId() || memcmp((void*)0x80000000, "SMN", 3)) {
		printf("This is not New Super Mario Bros. Wii\n");
		goto reinsert_disc;
	}

	if (!Launcher_ReadFST()) {
		printf("There was an error reading the disc. Press Home to exit, and try again.\n");
		while (true)
			HOME_EXIT();
	}

	printf("Press A to dump the bare minimum for Reggie! or B to dump the entire disc.\n");
	printf("Press 2 to dump entire disc and main.dol\n");
	int select = -1;
	while (select < 0) {
		HOME_EXIT();
		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_A)
			select = 0;
		else if (WPAD_ButtonsDown(0) & WPAD_BUTTON_B)
			select = 1;
		else if (WPAD_ButtonsDown(0) & WPAD_BUTTON_2)
			select = 2;
	}

	printf("Dumping files off of the disc...\n");
	mkdir("/reggie", 0777);

	if (select == 2)
		DumpMainDol();

	if (!DumpFolder(select ? "/" : "/Stage", select ? "/reggie" : "/reggie/stage")) {
		printf("\nThe files could not be read from disc. Press Home to exit, and try again.\n");
		while (true)
			HOME_EXIT();
	}

	fatUnmount("sd:");
	fatUnmount("usb:");

	printf("\nWe're all done. Enjoy Reggie!\n");
	printf("Press Home to exit.\n\n\n");
	while (true)
		HOME_EXIT();

	return 0;
}

