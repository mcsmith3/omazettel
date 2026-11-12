/*
 * omazettel.com: OmaZettel's command line on Windows.
 *
 * Windows runs a program either as a window app or as a terminal program,
 * not both. omazettel.exe is the window app, so on its own `omazettel tasks`
 * in a terminal would print nothing. Terminals look for omazettel.com before
 * omazettel.exe, and this runs omazettel.exe beside it with the terminal's
 * input and output, waits for it and hands back its exit code. Typed with no
 * arguments, it opens the window and returns at once.
 *
 * Built by bin/build: gcc -O2 -municode -o omazettel.com launcher.c
 */
#include <windows.h>
#include <wchar.h>

/* The command line after the program's own name, as Windows passed it. */
static const wchar_t *arguments(const wchar_t *line) {
    if (*line == L'"') {
        ++line;
        while (*line && *line != L'"')
            ++line;
        if (*line == L'"')
            ++line;
    } else {
        while (*line && *line != L' ' && *line != L'\t')
            ++line;
    }
    while (*line == L' ' || *line == L'\t')
        ++line;
    return line;
}

int wmain(void) {
    wchar_t program[MAX_PATH + 8];
    DWORD length = GetModuleFileNameW(NULL, program, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return 1;
    /* omazettel.com -> omazettel.exe */
    wchar_t *dot = wcsrchr(program, L'.');
    if (!dot)
        return 1;
    wcscpy(dot, L".exe");

    const wchar_t *rest = arguments(GetCommandLineW());
    const BOOL windowOnly = *rest == L'\0';
    size_t size = wcslen(program) + wcslen(rest) + 4;
    wchar_t *command = HeapAlloc(GetProcessHeap(), 0, size * sizeof(wchar_t));
    if (!command)
        return 1;
    wcscpy(command, L"\"");
    wcscat(command, program);
    wcscat(command, L"\"");
    if (*rest) {
        wcscat(command, L" ");
        wcscat(command, rest);
    }

    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    if (!windowOnly) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }

    /* Closing the terminal, or Ctrl+C here, also ends OmaZettel's command. */
    HANDLE job = NULL;
    if (!windowOnly) {
        job = CreateJobObjectW(NULL, NULL);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
            ZeroMemory(&limits, sizeof(limits));
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        }
    }

    /* OmaZettel writes UTF-8. */
    const UINT outputPage = GetConsoleOutputCP();
    const UINT inputPage = GetConsoleCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    PROCESS_INFORMATION process;
    if (!CreateProcessW(program, command, NULL, NULL, !windowOnly, CREATE_SUSPENDED, NULL, NULL, &startup,
                        &process)) {
        DWORD error = GetLastError();
        SetConsoleOutputCP(outputPage);
        SetConsoleCP(inputPage);
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        DWORD written;
        const char message[] = "omazettel: could not start omazettel.exe, which should be beside omazettel.com.\r\n";
        WriteFile(err, message, sizeof(message) - 1, &written, NULL);
        return error ? (int)error : 1;
    }
    if (job)
        AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);

    DWORD code = 0;
    if (!windowOnly) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &code);
    }
    CloseHandle(process.hProcess);
    SetConsoleOutputCP(outputPage);
    SetConsoleCP(inputPage);
    if (job)
        CloseHandle(job);
    return (int)code;
}
