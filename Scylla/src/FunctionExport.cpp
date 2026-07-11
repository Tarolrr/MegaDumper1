#include "FunctionExport.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>

#include "ApiReader.h"
#include "AppInfo.h"
#include "Architecture.h"
#include "IATSearch.h"
#include "ImportRebuilder.h"
#include "PeParser.h"
#include "ProcessAccessHelp.h"
#include "ProcessLister.h"
#include "Scylla.h"

extern HINSTANCE hDllModule;

int InitializeGui(HINSTANCE hInstance, LPARAM param);

// Lightweight diagnostic logger. Writes "scylla_native_log.txt" next to the
// loaded Scylla.dll so the native side can be traced even when a call faults
// (an AccessViolation on .NET 8 is uncatchable by the managed caller, so the
// managed log stops at "CALL ..."; this file shows how far the native code got).
static void ScyllaNativeLog(const char* fmt, ...) {
  char msg[1024];
  va_list args;
  va_start(args, fmt);
  _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);
  va_end(args);

  WCHAR path[MAX_PATH] = {0};
  if (GetModuleFileNameW(hDllModule, path, _countof(path)) != 0) {
    WCHAR* lastSlash = wcsrchr(path, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    wcsncat_s(path, _countof(path), L"scylla_native_log.txt", _TRUNCATE);
  } else {
    wcscpy_s(path, _countof(path), L"scylla_native_log.txt");
  }

  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"a") == 0 && f) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, msg);
    fclose(f);
  }
}

const WCHAR* WINAPI ScyllaVersionInformationW() {
  return APPNAME L" " ARCHITECTURE L" " APPVERSION;
}

const char* WINAPI ScyllaVersionInformationA() {
  return APPNAME_S " " ARCHITECTURE_S " " APPVERSION_S;
}

BOOL DumpProcessW(const WCHAR* fileToDump, DWORD_PTR imagebase,
                  DWORD_PTR entrypoint, const WCHAR* fileResult) {
  PeParser* peFile = 0;

  if (fileToDump) {
    peFile = new PeParser(fileToDump, true);
  } else {
    peFile = new PeParser(imagebase, true);
  }

  bool result = peFile->dumpProcess(imagebase, entrypoint, fileResult);

  delete peFile;
  return result;
}

BOOL WINAPI ScyllaRebuildFileW(const WCHAR* fileToRebuild, BOOL removeDosStub,
                               BOOL updatePeHeaderChecksum, BOOL createBackup) {
  if (createBackup) {
    if (!ProcessAccessHelp::createBackupFile(fileToRebuild)) {
      return FALSE;
    }
  }

  PeParser peFile(fileToRebuild, true);
  if (peFile.readPeSectionsFromFile()) {
    peFile.setDefaultFileAlignment();
    if (removeDosStub) {
      peFile.removeDosStub();
    }
    peFile.alignAllSectionHeaders();
    peFile.fixPeHeader();

    if (peFile.savePeFileToDisk(fileToRebuild)) {
      if (updatePeHeaderChecksum) {
        PeParser::updatePeHeaderChecksum(
            fileToRebuild,
            (DWORD)ProcessAccessHelp::getFileSize(fileToRebuild));
      }
      return TRUE;
    }
  }

  return FALSE;
}

BOOL WINAPI ScyllaRebuildFileA(const char* fileToRebuild, BOOL removeDosStub,
                               BOOL updatePeHeaderChecksum, BOOL createBackup) {
  WCHAR fileToRebuildW[MAX_PATH];
  if (MultiByteToWideChar(CP_ACP, 0, fileToRebuild, -1, fileToRebuildW,
                          _countof(fileToRebuildW)) == 0) {
    return FALSE;
  }

  return ScyllaRebuildFileW(fileToRebuildW, removeDosStub,
                            updatePeHeaderChecksum, createBackup);
}

BOOL WINAPI ScyllaDumpCurrentProcessW(const WCHAR* fileToDump,
                                      DWORD_PTR imagebase, DWORD_PTR entrypoint,
                                      const WCHAR* fileResult) {
  ProcessAccessHelp::setCurrentProcessAsTarget();

  return DumpProcessW(fileToDump, imagebase, entrypoint, fileResult);
}

BOOL WINAPI ScyllaDumpProcessW(DWORD_PTR pid, const WCHAR* fileToDump,
                               DWORD_PTR imagebase, DWORD_PTR entrypoint,
                               const WCHAR* fileResult) {
  ScyllaNativeLog("ScyllaDumpProcessW: pid=%u imagebase=0x%p entrypoint=0x%p",
                  (DWORD)pid, (void*)imagebase, (void*)entrypoint);
  if (ProcessAccessHelp::openProcessHandle((DWORD)pid)) {
    BOOL r = DumpProcessW(fileToDump, imagebase, entrypoint, fileResult);
    ScyllaNativeLog("ScyllaDumpProcessW: result=%d", r);
    return r;
  } else {
    ScyllaNativeLog("ScyllaDumpProcessW: openProcessHandle FAILED");
    return FALSE;
  }
}

BOOL WINAPI ScyllaDumpCurrentProcessA(const char* fileToDump,
                                      DWORD_PTR imagebase, DWORD_PTR entrypoint,
                                      const char* fileResult) {
  WCHAR fileToDumpW[MAX_PATH];
  WCHAR fileResultW[MAX_PATH];

  if (fileResult == 0) {
    return FALSE;
  }

  if (MultiByteToWideChar(CP_ACP, 0, fileResult, -1, fileResultW,
                          _countof(fileResultW)) == 0) {
    return FALSE;
  }

  if (fileToDump != 0) {
    if (MultiByteToWideChar(CP_ACP, 0, fileToDump, -1, fileToDumpW,
                            _countof(fileToDumpW)) == 0) {
      return FALSE;
    }

    return ScyllaDumpCurrentProcessW(fileToDumpW, imagebase, entrypoint,
                                     fileResultW);
  } else {
    return ScyllaDumpCurrentProcessW(0, imagebase, entrypoint, fileResultW);
  }
}

BOOL WINAPI ScyllaDumpProcessA(DWORD_PTR pid, const char* fileToDump,
                               DWORD_PTR imagebase, DWORD_PTR entrypoint,
                               const char* fileResult) {
  WCHAR fileToDumpW[MAX_PATH];
  WCHAR fileResultW[MAX_PATH];

  if (fileResult == 0) {
    return FALSE;
  }

  if (MultiByteToWideChar(CP_ACP, 0, fileResult, -1, fileResultW,
                          _countof(fileResultW)) == 0) {
    return FALSE;
  }

  if (fileToDump != 0) {
    if (MultiByteToWideChar(CP_ACP, 0, fileToDump, -1, fileToDumpW,
                            _countof(fileToDumpW)) == 0) {
      return FALSE;
    }

    return ScyllaDumpProcessW(pid, fileToDumpW, imagebase, entrypoint,
                              fileResultW);
  } else {
    return ScyllaDumpProcessW(pid, 0, imagebase, entrypoint, fileResultW);
  }
}

INT WINAPI ScyllaStartGui(DWORD dwProcessId, HINSTANCE mod,
                          DWORD_PTR entrypoint) {
  GUI_DLL_PARAMETER guiParam;
  guiParam.dwProcessId = dwProcessId;
  guiParam.mod = mod;
  guiParam.entrypoint = entrypoint;

  return InitializeGui(hDllModule, (LPARAM)&guiParam);
}

int WINAPI ScyllaIatSearch(DWORD dwProcessId, DWORD_PTR imagebase,
                           DWORD_PTR* iatStart, DWORD* iatSize,
                           DWORD_PTR searchStart, BOOL advancedSearch) {
  ScyllaNativeLog(
      "ScyllaIatSearch: pid=%u imagebase=0x%p searchStart=0x%p advanced=%d",
      dwProcessId, (void*)imagebase, (void*)searchStart, advancedSearch);
  try {
    ApiReader apiReader;
    ProcessLister processLister;
    Process* processPtr = 0;
    IATSearch iatSearch;

    std::vector<Process>& processList =
        processLister.getProcessListSnapshotNative();
    for (std::vector<Process>::iterator it = processList.begin();
         it != processList.end(); ++it) {
      if (it->PID == dwProcessId) {
        processPtr = &(*it);
        break;
      }
    }

    if (!processPtr) return SCY_ERROR_PIDNOTFOUND;

    ProcessAccessHelp::closeProcessHandle();
    apiReader.clearAll();

    if (!ProcessAccessHelp::openProcessHandle(processPtr->PID)) {
      return SCY_ERROR_PROCOPEN;
    }

    ProcessAccessHelp::getProcessModules(ProcessAccessHelp::hProcess,
                                         ProcessAccessHelp::moduleList);

    ProcessAccessHelp::selectedModule = 0;
    if (imagebase == 0 || imagebase == processPtr->imageBase) {
      ProcessAccessHelp::targetImageBase = processPtr->imageBase;
      ProcessAccessHelp::targetSizeOfImage = processPtr->imageSize;
    } else {
      auto module_it =
          std::find_if(ProcessAccessHelp::moduleList.cbegin(),
                       ProcessAccessHelp::moduleList.cend(),
                       [&](auto& mod) { return mod.modBaseAddr == imagebase; });
      if (module_it == ProcessAccessHelp::moduleList.cend()) {
        // Hidden module bypass with SAFE size detection
        ProcessAccessHelp::targetImageBase = imagebase;
        ProcessAccessHelp::targetSizeOfImage = ProcessAccessHelp::getSizeOfImageProcess(ProcessAccessHelp::hProcess, imagebase);
        if (ProcessAccessHelp::targetSizeOfImage == 0) ProcessAccessHelp::targetSizeOfImage = 0x2000000; // 32MB Fallback
      } else {
        ProcessAccessHelp::targetImageBase = module_it->modBaseAddr;
        ProcessAccessHelp::targetSizeOfImage = module_it->modBaseSize;
      }
    }

    apiReader.readApisFromModuleList();

    int retVal = SCY_ERROR_IATNOTFOUND;

    if (advancedSearch) {
      if (iatSearch.searchImportAddressTableInProcess(searchStart, iatStart,
                                                      iatSize, true)) {
        retVal = SCY_ERROR_SUCCESS;
      }
    } else {
      if (iatSearch.searchImportAddressTableInProcess(searchStart, iatStart,
                                                      iatSize, false)) {
        retVal = SCY_ERROR_SUCCESS;
      }
    }

    processList.clear();
    ProcessAccessHelp::closeProcessHandle();
    apiReader.clearAll();

    ScyllaNativeLog("ScyllaIatSearch: retVal=%d iatStart=0x%p iatSize=0x%X",
                    retVal, (void*)(iatStart ? *iatStart : 0),
                    iatSize ? *iatSize : 0);
    return retVal;
  } catch (...) {
    ScyllaNativeLog("ScyllaIatSearch: EXCEPTION caught -> SCY_ERROR_IATSEARCH");
    return SCY_ERROR_IATSEARCH;
  }
}

int WINAPI ScyllaIatFixAutoW(DWORD dwProcessId, DWORD_PTR imagebase,
                             DWORD_PTR iatAddr, DWORD iatSize,
                             BOOL createNewIat, const WCHAR* dumpFile,
                             const WCHAR* iatFixFile) {
  ScyllaNativeLog(
      "ScyllaIatFixAutoW: pid=%u imagebase=0x%p iatAddr=0x%p iatSize=0x%X "
      "createNewIat=%d",
      dwProcessId, (void*)imagebase, (void*)iatAddr, iatSize, createNewIat);
  // NOTE: this function previously had NO exception handling; a fault here
  // propagated out and killed the managed host. Wrapped so failures are logged
  // and reported as an error code instead of crashing.
  try {
    ApiReader apiReader;
    ProcessLister processLister;
    Process* processPtr = 0;
    std::map<DWORD_PTR, ImportModuleThunk> moduleList;

    std::vector<Process>& processList =
        processLister.getProcessListSnapshotNative();
    for (std::vector<Process>::iterator it = processList.begin();
         it != processList.end(); ++it) {
      if (it->PID == dwProcessId) {
        processPtr = &(*it);
        break;
      }
    }

    if (!processPtr) {
      ScyllaNativeLog("ScyllaIatFixAutoW: PID not found");
      return SCY_ERROR_PIDNOTFOUND;
    }

    ProcessAccessHelp::closeProcessHandle();
    apiReader.clearAll();

    if (!ProcessAccessHelp::openProcessHandle(processPtr->PID)) {
      ScyllaNativeLog("ScyllaIatFixAutoW: openProcessHandle FAILED");
      return SCY_ERROR_PROCOPEN;
    }

    ProcessAccessHelp::getProcessModules(ProcessAccessHelp::hProcess,
                                         ProcessAccessHelp::moduleList);

    ProcessAccessHelp::selectedModule = 0;
    if (imagebase == 0 || imagebase == processPtr->imageBase) {
      ProcessAccessHelp::targetImageBase = processPtr->imageBase;
      ProcessAccessHelp::targetSizeOfImage = processPtr->imageSize;
    } else {
      auto module_it =
          std::find_if(ProcessAccessHelp::moduleList.cbegin(),
                       ProcessAccessHelp::moduleList.cend(),
                       [&](auto& mod) { return mod.modBaseAddr == imagebase; });
      if (module_it == ProcessAccessHelp::moduleList.cend()) {
        // Hidden module bypass with SAFE size detection
        ProcessAccessHelp::targetImageBase = imagebase;
        ProcessAccessHelp::targetSizeOfImage = ProcessAccessHelp::getSizeOfImageProcess(ProcessAccessHelp::hProcess, imagebase);
        if (ProcessAccessHelp::targetSizeOfImage == 0) ProcessAccessHelp::targetSizeOfImage = 0x2000000; // 32MB Fallback
      } else {
        ProcessAccessHelp::targetImageBase = module_it->modBaseAddr;
        ProcessAccessHelp::targetSizeOfImage = module_it->modBaseSize;
      }
    }
    ScyllaNativeLog(
        "ScyllaIatFixAutoW: targetImageBase=0x%p targetSizeOfImage=0x%X",
        (void*)ProcessAccessHelp::targetImageBase,
        (DWORD)ProcessAccessHelp::targetSizeOfImage);

    apiReader.readApisFromModuleList();
    ScyllaNativeLog("ScyllaIatFixAutoW: readApisFromModuleList done");

    apiReader.readAndParseIAT(iatAddr, iatSize, moduleList);
    ScyllaNativeLog("ScyllaIatFixAutoW: readAndParseIAT done, modules=%zu",
                    moduleList.size());

    // add IAT section to dump
    ImportRebuilder importRebuild(dumpFile);
    // FIX: Force Output ImageBase to match Runtime Base (prevents crash on hidden modules)
    importRebuild.setImageBase(ProcessAccessHelp::targetImageBase);

    IATReferenceScan iatReferenceScan{};
    importRebuild.enableOFTSupport();
    if (createNewIat) {
      importRebuild.iatReferenceScan = &iatReferenceScan;
      importRebuild.enableNewIatInSection(iatAddr, iatSize);
    }

    int retVal = SCY_ERROR_IATWRITE;

    if (importRebuild.rebuildImportTable(iatFixFile, moduleList)) {
      retVal = SCY_ERROR_SUCCESS;
    }

    processList.clear();
    moduleList.clear();
    ProcessAccessHelp::closeProcessHandle();
    apiReader.clearAll();

    ScyllaNativeLog("ScyllaIatFixAutoW: retVal=%d", retVal);
    return retVal;
  } catch (...) {
    ScyllaNativeLog(
        "ScyllaIatFixAutoW: EXCEPTION caught -> SCY_ERROR_IATWRITE");
    return SCY_ERROR_IATWRITE;
  }
}
