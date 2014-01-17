// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome_elf/create_file/chrome_create_file.h"

#include <string>

#include "base/strings/string16.h"
#include "chrome_elf/chrome_elf_constants.h"
#include "chrome_elf/ntdll_cache.h"
#include "sandbox/win/src/nt_internals.h"

namespace {

// From ShlObj.h in the Windows SDK.
#define CSIDL_LOCAL_APPDATA 0x001c

typedef BOOL (WINAPI *PathIsUNCFunction)(
  IN LPCWSTR path);

typedef BOOL (WINAPI *PathAppendFunction)(
  IN LPWSTR path,
  IN LPCWSTR more);

typedef BOOL (WINAPI *PathIsPrefixFunction)(
  IN LPCWSTR prefix,
  IN LPCWSTR path);

typedef HRESULT (WINAPI *SHGetFolderPathFunction)(
  IN HWND hwnd_owner,
  IN int folder,
  IN HANDLE token,
  IN DWORD flags,
  OUT LPWSTR path);

PathIsUNCFunction g_path_is_unc_func;
PathAppendFunction g_path_append_func;
PathIsPrefixFunction g_path_is_prefix_func;
SHGetFolderPathFunction g_get_folder_func;

// Populates the g_*_func pointers to functions which will be used in
// ShouldBypass(). Chrome_elf cannot have a load-time dependency on shell32 or
// shlwapi as this would induce a load-time dependency on user32.dll. Instead,
// the addresses of the functions we need are retrieved the first time this
// method is called, and cached to avoid subsequent calls to GetProcAddress().
// It is assumed that the host process will never unload these functions.
// Returns true if all the functions needed are present.
bool PopulateShellFunctions() {
  // Early exit if functions have already been populated.
  if (g_path_is_unc_func && g_path_append_func &&
      g_path_is_prefix_func && g_get_folder_func) {
    return true;
  }

  // Get the addresses of the functions we need and store them for future use.
  // These handles are intentionally leaked to ensure that these modules do not
  // get unloaded.
  HMODULE shell32 = ::LoadLibrary(L"shell32.dll");
  HMODULE shlwapi = ::LoadLibrary(L"shlwapi.dll");

  if (!shlwapi || !shell32)
    return false;

  g_path_is_unc_func = reinterpret_cast<PathIsUNCFunction>(
      ::GetProcAddress(shlwapi, "PathIsUNCW"));
  g_path_append_func = reinterpret_cast<PathAppendFunction>(
      ::GetProcAddress(shlwapi, "PathAppendW"));
  g_path_is_prefix_func = reinterpret_cast<PathIsPrefixFunction>(
      ::GetProcAddress(shlwapi, "PathIsPrefixW"));
  g_get_folder_func = reinterpret_cast<SHGetFolderPathFunction>(
      ::GetProcAddress(shell32, "SHGetFolderPathW"));

  return g_path_is_unc_func && g_path_append_func &&
      g_path_is_prefix_func && g_get_folder_func;
}

}  // namespace

HANDLE WINAPI CreateFileWRedirect(
    LPCWSTR file_name,
    DWORD desired_access,
    DWORD share_mode,
    LPSECURITY_ATTRIBUTES security_attributes,
    DWORD creation_disposition,
    DWORD flags_and_attributes,
    HANDLE template_file) {
  if (ShouldBypass(file_name)) {
    return CreateFileNTDLL(file_name,
                           desired_access,
                           share_mode,
                           security_attributes,
                           creation_disposition,
                           flags_and_attributes,
                           template_file);
  }
  return CreateFile(file_name,
                    desired_access,
                    share_mode,
                    security_attributes,
                    creation_disposition,
                    flags_and_attributes,
                    template_file);

}

HANDLE CreateFileNTDLL(
    LPCWSTR file_name,
    DWORD desired_access,
    DWORD share_mode,
    LPSECURITY_ATTRIBUTES security_attributes,
    DWORD creation_disposition,
    DWORD flags_and_attributes,
    HANDLE template_file) {
  HANDLE file_handle = INVALID_HANDLE_VALUE;
  NTSTATUS result = STATUS_UNSUCCESSFUL;
  IO_STATUS_BLOCK io_status_block = {};

  // Convert from Win32 domain to to NT creation disposition values.
  switch (creation_disposition) {
    case CREATE_NEW:
      creation_disposition = FILE_CREATE;
      break;
    case CREATE_ALWAYS:
      creation_disposition = FILE_OVERWRITE_IF;
      break;
    case OPEN_EXISTING:
      creation_disposition = FILE_OPEN;
      break;
    case OPEN_ALWAYS:
      creation_disposition = FILE_OPEN_IF;
      break;
    case TRUNCATE_EXISTING:
      creation_disposition = FILE_OVERWRITE;
      break;
    default:
      SetLastError(ERROR_INVALID_PARAMETER);
      return INVALID_HANDLE_VALUE;
  }

  if (!g_ntdll_lookup["NtCreateFile"] ||
      !g_ntdll_lookup["RtlInitUnicodeString"]) {
    return INVALID_HANDLE_VALUE;
  }

  NtCreateFileFunction create_file =
      reinterpret_cast<NtCreateFileFunction>(g_ntdll_lookup["NtCreateFile"]);

  RtlInitUnicodeStringFunction init_unicode_string =
      reinterpret_cast<RtlInitUnicodeStringFunction>(
          g_ntdll_lookup["RtlInitUnicodeString"]);

  UNICODE_STRING path_unicode_string;

  // Format the path into an NT path. Arguably this should be done with
  // RtlDosPathNameToNtPathName_U, but afaict this is equivalent for
  // local paths. Using this with a UNC path name will almost certainly
  // break in interesting ways.
  base::string16 filename_string(L"\\??\\");
  filename_string += file_name;

  init_unicode_string(&path_unicode_string, filename_string.c_str());

  OBJECT_ATTRIBUTES path_attributes = {};
  InitializeObjectAttributes(&path_attributes,
                             &path_unicode_string,
                             OBJ_CASE_INSENSITIVE,
                             NULL,   // No Root Directory
                             NULL);  // No Security Descriptor

  // Set create_options, desired_access, and flags_and_attributes to match those
  // set by kernel32!CreateFile.
  ULONG create_options = FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_ARCHIVE;
  desired_access |= 0x100080;
  flags_and_attributes &= 0x2FFA7;

  result = create_file(&file_handle,
                       desired_access,
                       &path_attributes,
                       &io_status_block,
                       0,  // Allocation size
                       flags_and_attributes,
                       share_mode,
                       creation_disposition,
                       create_options,
                       NULL,
                       0);

  if (result != STATUS_SUCCESS) {
    if (result == STATUS_OBJECT_NAME_COLLISION &&
        creation_disposition == FILE_CREATE) {
      SetLastError(ERROR_FILE_EXISTS);
    }
    return INVALID_HANDLE_VALUE;
  }

  if (creation_disposition == FILE_OPEN_IF) {
    SetLastError(io_status_block.Information == FILE_OPENED ?
        ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
  } else if (creation_disposition == FILE_OVERWRITE_IF) {
    SetLastError(io_status_block.Information == FILE_OVERWRITTEN ?
        ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
  } else {
    SetLastError(ERROR_SUCCESS);
  }

  return file_handle;
}

bool ShouldBypass(LPCWSTR file_name) {
  // If the shell functions are not present, forward the call to kernel32.
  if (!PopulateShellFunctions())
    return false;

  // Forward all UNC filepaths to kernel32.
  if (g_path_is_unc_func(file_name))
    return false;

  wchar_t local_appdata_path[MAX_PATH];

  // Get the %LOCALAPPDATA% Path and append the location of our UserData
  // directory to it.
  HRESULT appdata_result = g_get_folder_func(
      NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local_appdata_path);

  // If getting the %LOCALAPPDATA% path or appending to it failed, then forward
  // the call to kernel32.
  if (!SUCCEEDED(appdata_result) ||
      !g_path_append_func(local_appdata_path, kAppDataDirName) ||
      !g_path_append_func(local_appdata_path, kUserDataDirName)) {
    return false;
  }

  // Check if we are trying to access something in the UserData dir. If so,
  // then redirect the call to bypass kernel32.
  return !!g_path_is_prefix_func(local_appdata_path, file_name);
}