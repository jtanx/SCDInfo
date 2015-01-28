#include <windows.h>
#include <ntddcdrm.h>
#include <Commctrl.h>
#include <strsafe.h>
#include <process.h>

#include "resource.h"

#define IDM_THREADFINISH   12130

#define UDF_OFFSET ((64 << 10) + 376)
#define UDF_SIZE 12

#define ISO9960_OFFSET ((32 << 10) + 813)
#define ISO9960_SIZE 17

#define FmtError(err, buf, bufsize) \
	FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err, 0, buf, bufsize, NULL)
#define GetLastErrorStr(buf, bufsize) FmtError(GetLastError(), buf, bufsiz)                        
#define ERRMSG(hwnd, msg) MessageBox(hwnd, msg, NULL, MB_ICONEXCLAMATION | MB_OK)

typedef struct TimeInfo {
	const wchar_t *description;
	int year;
	int month;
	int day;
	int hour;
	int minute;
	int second;
	int microsecond;
	int offset;
} TimeInfo;

typedef struct WorkerInfo {
	HWND hwndMain;
	wchar_t drive[4];
} WorkerInfo;


/** 
 * Converts an ascii representation of a number to its integer (Base 10) representation.
 * The number must be (a.) positive, and (b.) contain only digits (0-9).
 */
int ReadASCIIInt(LPBYTE pt, size_t offset, size_t length)
{
	int ret = 0;

	for (size_t i = 0; i < length; i++) {
		ret = ret * 10 + pt[offset + i] - '0';
	}
	return ret;
}

/**
 * Reads an ISO 9960 PVD formatted date.
 */
TimeInfo ReadISO9960Date(BYTE rawdate[ISO9960_SIZE], const wchar_t *description)
{
	TimeInfo ti;
	
	//8 bit 2's complement in 15 minute intervals
	ti.offset = rawdate[16];
	ti.offset = ((ti.offset & 0x80) ? ti.offset - 0x100 : ti.offset) * 15;
	ti.year = ReadASCIIInt(rawdate, 0, 4);
	ti.month = ReadASCIIInt(rawdate, 4, 2);
	ti.day = ReadASCIIInt(rawdate, 6, 2);
	ti.hour = ReadASCIIInt(rawdate, 8, 2);
	ti.minute = ReadASCIIInt(rawdate, 10, 2);
	ti.second = ReadASCIIInt(rawdate, 12, 2);
	ti.microsecond = ReadASCIIInt(rawdate, 14, 2) * 10000;

	ti.description = description;
	return ti;
}

/**
 * Reads a UDF PVD formatted date.
 */
TimeInfo ReadUDFDate(BYTE rawdate[UDF_SIZE], const wchar_t *description)
{
	TimeInfo ti;

	//12 bit LE 2's complement
	ti.offset = ((rawdate[1] & 0x0F) << 8) | rawdate[0];
	ti.offset = (ti.offset & 0x800) ? ti.offset - 0x1000 : ti.offset;
	//16 bit LE 2's complement
	ti.year = (rawdate[3] << 8) | rawdate[2];
	ti.year = (ti.year & 0x8000) ? ti.year - 0x10000 : ti.year;
	ti.month = rawdate[4];
	ti.day = rawdate[5];
	ti.hour = rawdate[6];
	ti.minute = rawdate[7];
	ti.second = rawdate[8];
	ti.microsecond = rawdate[9] * 10000 + rawdate[10] * 100 + rawdate[11];

	ti.description = description;
	return ti;
}

/**
 * Retrieves the specified bytes from the CD.
 * Based on code from http://support.microsoft.com/kb/KbView/138434
 */
static DWORD GetCDBytes(wchar_t driveletter, DWORD offset, DWORD length, LPBYTE buffer)
{
	HANDLE  hCD;
	DWORD   dwNotUsed;
	DWORD   ret = ERROR_SUCCESS;
	wchar_t ntpath[7] = { L'\\', L'\\', L'.', L'\\', driveletter, L':', 0 };

	hCD = CreateFile(ntpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
		NULL);

	if (hCD != INVALID_HANDLE_VALUE) {
		DISK_GEOMETRY         dgCDROM;
		PREVENT_MEDIA_REMOVAL pmrLockCDROM;

		// Lock the compact disc in the CD-ROM drive to prevent accidental
		// removal while reading from it.
		pmrLockCDROM.PreventMediaRemoval = TRUE;
		DeviceIoControl(hCD, IOCTL_CDROM_MEDIA_REMOVAL,
			&pmrLockCDROM, sizeof(pmrLockCDROM), NULL, 0, &dwNotUsed, NULL);

		// Get sector size of compact disc
		if (DeviceIoControl(hCD, IOCTL_CDROM_GET_DRIVE_GEOMETRY,
			NULL, 0, &dgCDROM, sizeof(dgCDROM),                                
			&dwNotUsed, NULL))
		{
			LPBYTE lpSector;
			DWORD  isector = (offset / dgCDROM.BytesPerSector) * dgCDROM.BytesPerSector;
			DWORD  dwSize  = (offset + length) - isector;
			DWORD  remainder = dwSize % dgCDROM.BytesPerSector;

			dwSize = dwSize + dgCDROM.BytesPerSector - remainder;
			lpSector = (LPBYTE)VirtualAlloc(NULL, dwSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

			if (lpSector != NULL) {
				// Move it to the starting sector for the given offset.
				if (SetFilePointer(hCD, isector, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
					// Read sectors from the compact disc and copy them to the output buffer
					if (ReadFile(hCD, lpSector, dwSize, &dwNotUsed, NULL)) {
						memcpy(buffer, lpSector + (offset - isector), length);
					} else {
						ret = GetLastError();
					}
				} else {
					ret = GetLastError();
				}
				VirtualFree(lpSector, 0, MEM_RELEASE);
			} else {
				ret = GetLastError();
			}
		} else {
			ret = GetLastError();
		}

		// Unlock the disc in the CD-ROM drive.
		pmrLockCDROM.PreventMediaRemoval = FALSE;
		DeviceIoControl(hCD, IOCTL_CDROM_MEDIA_REMOVAL,
			&pmrLockCDROM, sizeof(pmrLockCDROM), NULL, 0, &dwNotUsed, NULL);

		CloseHandle(hCD);
	} else {
		ret = GetLastError();
	}

	return ret;
}

/**
 * Get the CD info for the selected drive.
 */
static void GetDriveInfo(HWND hwnd, wchar_t drive[4])
{
	wchar_t filetype[MAX_PATH + 1], buf[BUFSIZ];
	TimeInfo ti[4];
	HWND hwndInfo = GetDlgItem(hwnd, IDC_INFO);
	DWORD ret = ERROR_SUCCESS;
	int numTimes = 0;

	if (!GetVolumeInformation(drive, NULL, 0, NULL, NULL, NULL, filetype, MAX_PATH)) {
		ret = GetLastError();
	} else if (!wcscmp(L"UDF", filetype)) {
		BYTE rawdate[UDF_SIZE];
		ret = GetCDBytes(drive[0], UDF_OFFSET, UDF_SIZE, rawdate);
		if (ret == ERROR_SUCCESS) {
			ti[0] = ReadUDFDate(rawdate, L"Date created");
			numTimes = 1;
		}
	} else if (!wcscmp(L"CDFS", filetype)) {
		BYTE rawdates[ISO9960_SIZE * 4];
		const wchar_t *descriptors[] = { 
			L"Date created", L"Date modified", L"Expiration date", L"Use from date"
		};

		ret = GetCDBytes(drive[0], ISO9960_OFFSET, ISO9960_SIZE * 4, rawdates);
		if (ret == ERROR_SUCCESS) {
			for (int i = 0; i < 4; i++) {
				ti[i] = ReadISO9960Date(rawdates + i * ISO9960_SIZE, descriptors[i]);
			}
			numTimes = 4;
		}
	} else {
		StringCchPrintf(buf, BUFSIZ, L"Unsupported file system: %s (UDF and ISO-9660/CDFS only).",
			filetype);
		SetWindowText(hwndInfo, buf);
	}

	if (numTimes > 0) {
		wchar_t line[BUFSIZ];
		
		StringCchPrintf(buf, BUFSIZ,
			L"File system: %s\r\nDate format: DD/MM/YYYY HH:MM:SS.CC\r\n\r\n", filetype);
		for (int i = 0; i < numTimes; i++) {
			StringCchPrintf(line, BUFSIZ, L"%s: %02d/%02d/%04d %02d:%02d:%02d.%02d UTC%+.1f\r\n",
				ti[i].description, ti[i].day, ti[i].month, ti[i].year,
				ti[i].hour, ti[i].minute, ti[i].second, ti[i].microsecond / 10000,
				ti[i].offset / 60.0f);
			StringCchCat(buf, BUFSIZ, line);
		}
		SetWindowText(hwndInfo, buf);
	} else if (FAILED(HRESULT_FROM_WIN32(ret))) {
		wchar_t err[BUFSIZ];
		FmtError(ret, err, BUFSIZ);
		StringCchPrintf(buf, BUFSIZ, L"Error:\r\n%s", err);
		SetWindowText(hwndInfo, buf);
	}
}

/** 
 * The thread that reads off the CD drive. Used so the UI doesn't freeze up when it takes
 * a while to read off the CD.
 */
unsigned __stdcall WorkerThread(void *arg)
{
	WorkerInfo *wi = (WorkerInfo*)arg;
	EnableWindow(GetDlgItem(wi->hwndMain, IDC_RESCAN), FALSE);
	EnableWindow(GetDlgItem(wi->hwndMain, IDC_DRIVE), FALSE);
	SetDlgItemText(wi->hwndMain, IDC_INFO, L"Reading...");
	GetDriveInfo(wi->hwndMain, wi->drive);
	EnableWindow(GetDlgItem(wi->hwndMain, IDC_RESCAN), TRUE);
	EnableWindow(GetDlgItem(wi->hwndMain, IDC_DRIVE), TRUE);
	PostMessage(wi->hwndMain, WM_COMMAND, MAKELONG(IDM_THREADFINISH, 0), 0);

	return 0;
}

/**
 * Callback procedure for the main window.
 */
INT_PTR CALLBACK MainDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static WorkerInfo wi = { 0 };
	static uintptr_t thread = 0;

	switch (msg) {
		case WM_INITDIALOG:
			//Set dialogue icons
			SendMessage(hwnd, WM_SETICON, ICON_SMALL,
				(LPARAM)LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON)));
			SendMessage(hwnd, WM_SETICON, ICON_BIG,
				(LPARAM)LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON)));
			//Scan for CD drives
			SendMessage(hwnd, WM_COMMAND, MAKELONG(IDC_RESCAN, 0), 0);
		break;
		case WM_COMMAND:
			switch (LOWORD(wParam)) {
				case IDC_RESCAN: { //Update drive list and CD info.
					HWND hwndDrive = GetDlgItem(hwnd, IDC_DRIVE);
					DWORD drives = GetLogicalDrives();
					DWORD cur = SendMessage(hwndDrive, CB_GETCURSEL, 0, 0); //Current selection
					int toSelect = 0; //The list index of the drive to select

					//Determine what drive is currently selected (if any)
					if (cur != CB_ERR) {
						wchar_t drive[4] = { 0 };
						SendMessage(hwndDrive, CB_GETLBTEXT, cur, (LPARAM)drive);
						cur = drive[0] - L'A';
					}

					SendMessage(hwndDrive, CB_RESETCONTENT, 0, 0);
					for (int i = 0; i < 26; i++) {
						if (drives & (1 << i)) {
							wchar_t drive[4] = { L'A' + i, L':', L'\\', 0 };
							if (GetDriveType(drive) == DRIVE_CDROM) {
								int idx = SendMessage(hwndDrive, CB_ADDSTRING, 0, (LPARAM)drive);
								//Select the previously selected drive (if any)
								if (cur == i && idx >= 0) {
									toSelect = idx;
								}
							}
						}
					}
					//Set the selected drive
					SendMessage(hwndDrive, CB_SETCURSEL, toSelect, 0);
					//Re-retrieve the CD info
					SendMessage(hwnd, WM_COMMAND, MAKELONG(IDC_DRIVE, CBN_SELCHANGE), (LPARAM)hwndDrive);
				}
				break;

				case IDC_DRIVE: { //Get the CD info
					if (HIWORD(wParam) == CBN_SELCHANGE) {
						DWORD idx = SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
						if (idx != CB_ERR && !thread) {
							wi.hwndMain = hwnd;
							wi.drive[0] = 0;
							SendMessage((HWND)lParam, CB_GETLBTEXT, idx, (LPARAM)wi.drive);
							thread = _beginthreadex(NULL, 0, WorkerThread, &wi, 0, NULL);
						} else {
							SetDlgItemText(hwnd, IDC_INFO, L"");
						}
					}
				} break;

				case IDM_THREADFINISH: {
					thread = 0;
				} break;

				case IDOK: case IDCANCEL:
					EndDialog(hwnd, LOWORD(wParam));
				break;
			}
		break;

		default:
			return FALSE;
	}
	return TRUE;
}

/**
 * Program entry point.
 */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR lpCmdLine, int nCmdShow)
{
	INITCOMMONCONTROLSEX icc;

	icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
	icc.dwICC = ICC_STANDARD_CLASSES;
	if (!InitCommonControlsEx(&icc)){
		ERRMSG(NULL, L"Could not initialise common controls.");
		return 1;
	}

	//Disable 'Select a drive' from showing if the drive's empty.
	SetErrorMode(SEM_FAILCRITICALERRORS);

	if (!DialogBox(hInst, MAKEINTRESOURCE(IDD_MAIN), NULL, MainDlgProc)){
		ERRMSG(NULL, L"Could not initialise main window.");
		return 1;
	}
	return 0;
}