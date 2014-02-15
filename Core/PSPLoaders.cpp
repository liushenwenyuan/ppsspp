// Copyright (C) 2012 PPSSPP Project

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "file/file_util.h"

#include "Common/StringUtils.h"

#include "Core/ELF/ElfReader.h"
#include "Core/ELF/ParamSFO.h"

#include "FileSystems/BlockDevices.h"
#include "FileSystems/DirectoryFileSystem.h"
#include "FileSystems/ISOFileSystem.h"
#include "FileSystems/MetaFileSystem.h"
#include "FileSystems/VirtualDiscFileSystem.h"

#include "Core/MemMap.h"

#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSAnalyst.h"
#include "Core/MIPS/MIPSCodeUtils.h"

#include "Host.h"

#include "Core/Config.h"
#include "Core/System.h"
#include "Core/PSPLoaders.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceKernelModule.h"
#include "Core/HLE/sceKernelMemory.h"

// We gather the game info before actually loading/booting the ISO
// to determine if the emulator should enable extra memory and
// double-sized texture coordinates.
void InitMemoryForGameISO(std::string fileToStart) {
	IFileSystem* umd2;

	// check if it's a disc directory
	FileInfo info;
	if (!getFileInfo(fileToStart.c_str(), &info)) return;

	if (info.isDirectory)
	{
		umd2 = new VirtualDiscFileSystem(&pspFileSystem, fileToStart);
	}
	else 
	{
		auto bd = constructBlockDevice(fileToStart.c_str());
		// Can't init anything without a block device...
		if (!bd)
			return;
		umd2 = new ISOFileSystem(&pspFileSystem, bd);
	}

	// Parse PARAM.SFO

	//pspFileSystem.Mount("host0:",umd2);
	pspFileSystem.Mount("umd0:", umd2);
	pspFileSystem.Mount("umd1:", umd2);
	pspFileSystem.Mount("disc0:", umd2);
	pspFileSystem.Mount("umd:", umd2);

	std::string gameID;

	std::string sfoPath("disc0:/PSP_GAME/PARAM.SFO");
	PSPFileInfo fileInfo = pspFileSystem.GetFileInfo(sfoPath.c_str());

	if (fileInfo.exists)
	{
		std::vector<u8> paramsfo;
		pspFileSystem.ReadEntireFile(sfoPath, paramsfo);
		if (g_paramSFO.ReadSFO(paramsfo))
		{
			gameID = g_paramSFO.GetValueString("DISC_ID");

			for (size_t i = 0; i < ARRAY_SIZE(g_HDRemasters); i++) {
				if(g_HDRemasters[i].gameID == gameID) {
					g_RemasterMode = true;
					Memory::g_MemorySize = g_HDRemasters[i].MemorySize;
					if(g_HDRemasters[i].DoubleTextureCoordinates)
						g_DoubleTextureCoordinates = true;
					break;
				}
			}
			DEBUG_LOG(LOADER, "HDRemaster mode is %s", g_RemasterMode? "true": "false");
		}
	}
}


// Chinese translators like to rename EBOOT.BIN and replace it with some kind of stub
// that probably loads a plugin and then launches the actual game. These stubs don't work in PPSSPP.
// No idea why they are doing this, but it works to just bypass it. They could stop
// inventing new filenames though...
static const char *altBootNames[] = {
	"disc0:/PSP_GAME/SYSDIR/EBOOT.OLD",
	"disc0:/PSP_GAME/SYSDIR/EBOOT.OLD",
	"disc0:/PSP_GAME/SYSDIR/EBOOT.DAT",
	"disc0:/PSP_GAME/SYSDIR/EBOOT.BI",
	"disc0:/PSP_GAME/SYSDIR/EBOOT.LLD",
	"disc0:/PSP_GAME/SYSDIR/OLD_EBOOT.BIN",
	"disc0:/PSP_GAME/SYSDIR/EBOOT.123",
	"disc0:/PSP_GAME/SYSDIR/EBOOT_LRC_CH.BIN",
	"disc0:/PSP_GAME/SYSDIR/BOOT0.OLD",
	"disc0:/PSP_GAME/SYSDIR/BOOT1.OLD",
	"disc0:/PSP_GAME/SYSDIR/BINOT.BIN",
	"disc0:/PSP_GAME/SYSDIR/EBOOT.FRY",
	"disc0:/PSP_GAME/SYSDIR/EBOOT.Z.Y",
	"disc0:/PSP_GAME/SYSDIR/EBOOT.LEI",
	"disc0:/PSP_GAME/SYSDIR/EBOOT.DNR",
	"disc0:/PSP_GAME/SYSDIR/DBZ2.BIN",
	"disc0:/PSP_GAME/SYSDIR/PAKFILE2.BIN",
};

bool Load_PSP_ISO(const char *filename, std::string *error_string)
{
	// Mounting stuff relocated to InitMemoryForGameISO due to HD Remaster restructuring of code.

	std::string sfoPath("disc0:/PSP_GAME/PARAM.SFO");
	PSPFileInfo fileInfo = pspFileSystem.GetFileInfo(sfoPath.c_str());
	if (fileInfo.exists)
	{
		std::vector<u8> paramsfo;
		pspFileSystem.ReadEntireFile(sfoPath, paramsfo);
		if (g_paramSFO.ReadSFO(paramsfo))
		{
			char title[1024];
			sprintf(title, "%s : %s", g_paramSFO.GetValueString("DISC_ID").c_str(), g_paramSFO.GetValueString("TITLE").c_str());
			INFO_LOG(LOADER, "%s", title);
			host->SetWindowTitle(title);
		}
	}


	std::string bootpath("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");

	// Bypass Chinese translation patches, see comment above.
	for (int i = 0; i < ARRAY_SIZE(altBootNames); i++) {
		if (pspFileSystem.GetFileInfo(altBootNames[i]).exists) {
			bootpath = "disc0:/PSP_GAME/SYSDIR/EBOOT.OLD";
			break;
		}
	}

	bool hasEncrypted = false;
	u32 fd;
	if ((fd = pspFileSystem.OpenFile(bootpath, FILEACCESS_READ)) != 0)
	{
		u8 head[4];
		pspFileSystem.ReadFile(fd, head, 4);
		if (memcmp(head, "~PSP", 4) == 0 || memcmp(head, "\x7F""ELF", 4) == 0) {
			hasEncrypted = true;
		}
		pspFileSystem.CloseFile(fd);
	}
	if (!hasEncrypted)
	{
		// try unencrypted BOOT.BIN
		bootpath = "disc0:/PSP_GAME/SYSDIR/BOOT.BIN";
	}

	INFO_LOG(LOADER,"Loading %s...", bootpath.c_str());
	return __KernelLoadExec(bootpath.c_str(), 0, error_string);
}

bool Load_PSP_ELF_PBP(const char *filename, std::string *error_string)
{
	// This is really just for headless, might need tweaking later.
	if (!PSP_CoreParameter().mountIso.empty())
	{
		auto bd = constructBlockDevice(PSP_CoreParameter().mountIso.c_str());
		if (bd != NULL) {
			ISOFileSystem *umd2 = new ISOFileSystem(&pspFileSystem, bd);

			pspFileSystem.Mount("umd1:", umd2);
			pspFileSystem.Mount("disc0:", umd2);
			pspFileSystem.Mount("umd:", umd2);
		}
	}

	std::string full_path = filename;
	std::string path, file, extension;
	SplitPath(ReplaceAll(full_path, "\\", "/"), &path, &file, &extension);
#ifdef _WIN32
	path = ReplaceAll(path, "/", "\\");
#endif

	DirectoryFileSystem *fs = new DirectoryFileSystem(&pspFileSystem, path);
	pspFileSystem.Mount("umd0:", fs);

	std::string finalName = "umd0:/" + file + extension;
	return __KernelLoadExec(finalName.c_str(), 0, error_string);
}
