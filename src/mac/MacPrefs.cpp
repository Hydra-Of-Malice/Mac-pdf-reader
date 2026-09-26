/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/SettingsUtil.h"

#define INCLUDE_SETTINGSSTRUCTS_METADATA
#include "Settings.h"
#include "DisplayMode.h"
#include "mac/MacPrefs.h"

// the Windows app's settings file format (Settings.h), without AppSettings.cpp
// and the Win32 UI it depends on
static Settings* gGlobalPrefs = nullptr;
static Str gSettingsPath;
static Str gPreviousSettings;
static SessionData* gBuildingSession = nullptr;

static FileState* NewFileState(Str filePath) {
    auto* fs = (FileState*)DeserializeStruct(&gFileStateInfo, {});
    str::ReplaceWithCopy(&fs->filePath, filePath);
    return fs;
}

static Favorite* NewFavorite(int pageNo) {
    auto* fav = (Favorite*)DeserializeStruct(&gFavoriteInfo, {});
    str::ReplaceWithCopy(&fav->pageNo, fmt("%d", pageNo));
    return fav;
}

// FileState / TabState / Favorite PageNo: a page number, or "bm:<bookmark>"
// written by the Windows app for chaptered documents
static int PageNoFromStored(Str stored) {
    if (str::StartsWith(stored, StrL("bm:"))) {
        return 1;
    }
    return std::max(ParseInt(stored), 1);
}

static char* CopyCString(Str value) {
    char* result = (char*)malloc((size_t)len(value) + 1);
    if (result) {
        memcpy(result, value.s, (size_t)len(value));
        result[len(value)] = 0;
    }
    return result;
}

static FileState* FindFileState(Str path) {
    if (!gGlobalPrefs || !gGlobalPrefs->fileStates) {
        return nullptr;
    }
    for (FileState* state : *gGlobalPrefs->fileStates) {
        if (str::Eq(state->filePath, path)) {
            return state;
        }
    }
    return nullptr;
}

static void MoveFileStateToFront(FileState* state) {
    Vec<FileState*>& states = *gGlobalPrefs->fileStates;
    VecRemove(states, state);
    VecInsertAt(states, 0, state);
}

static void StateFromFileState(FileState* fileState, MacPrefsViewState* state) {
    *state = {};
    if (!fileState || fileState->useDefaultState) {
        return;
    }
    DisplayMode mode = DisplayModeFromString(fileState->displayMode, DisplayMode::Continuous);
    state->valid = true;
    state->continuous = IsContinuous(mode);
    state->zoomVirtual = ZoomFromString(fileState->zoom, kZoomFitWidth);
    state->rotation = fileState->rotation;
    state->pageNo = PageNoFromStored(fileState->pageNo);
}

static void SaveState(FileState* fileState, const MacPrefsViewState* state) {
    if (!fileState || !state) {
        return;
    }
    fileState->useDefaultState = false;
    DisplayMode mode = state->continuous ? DisplayMode::Continuous : DisplayMode::SinglePage;
    str::ReplaceWithCopy(&fileState->displayMode, DisplayModeToString(mode));
    str::ReplaceWithCopy(&fileState->pageNo, fmt("%d", state->pageNo));
    ZoomToString(&fileState->zoom, (float)state->zoomVirtual, fileState);
    fileState->rotation = state->rotation;
}

static SessionData* SavedSession() {
    if (!gGlobalPrefs || !gGlobalPrefs->restoreSession || !gGlobalPrefs->rememberOpenedFiles ||
        !gGlobalPrefs->sessionData || len(*gGlobalPrefs->sessionData) == 0) {
        return nullptr;
    }
    return (*gGlobalPrefs->sessionData)[0];
}

void MacPrefsInit(const char* settingsPath) {
    if (gGlobalPrefs) {
        return;
    }
    gSettingsPath = str::Dup(Str((char*)settingsPath));
    gPreviousSettings = file::ReadFile(gSettingsPath);
    gGlobalPrefs = (Settings*)DeserializeStruct(&gSettingsInfo, gPreviousSettings);
}

void MacPrefsShutdown() {
    if (gGlobalPrefs && gSettingsPath) {
        Str serialized = SerializeStruct(&gSettingsInfo, gGlobalPrefs, gPreviousSettings);
        if (dir::CreateForFile(gSettingsPath) && file::WriteFile(gSettingsPath, serialized)) {
            str::Free(gPreviousSettings);
            gPreviousSettings = str::Dup(serialized);
        }
        str::Free(serialized);
    }
    if (gGlobalPrefs) {
        FreeStruct(&gSettingsInfo, gGlobalPrefs);
    }
    gGlobalPrefs = nullptr;
    str::Free(gPreviousSettings);
    str::Free(gSettingsPath);
}

bool MacPrefsOpenDocument(const char* path, MacPrefsViewState* state) {
    if (!path || !state || !gGlobalPrefs) {
        return false;
    }
    Str filePath((char*)path);
    FileState* fileState = FindFileState(filePath);
    StateFromFileState(fileState, state);
    if (!fileState) {
        fileState = NewFileState(filePath);
    }
    MoveFileStateToFront(fileState);
    fileState->isMissing = false;
    fileState->openCount++;
    return state->valid;
}

void MacPrefsSaveDocument(const char* path, const MacPrefsViewState* state) {
    if (!path || !state || !gGlobalPrefs) {
        return;
    }
    Str filePath((char*)path);
    FileState* fileState = FindFileState(filePath);
    if (!fileState) {
        fileState = NewFileState(filePath);
        MoveFileStateToFront(fileState);
    }
    SaveState(fileState, state);
}

void MacPrefsSaveSession(const char* path, const MacPrefsViewState* state) {
    MacPrefsBeginSession();
    MacPrefsAppendSession(path, state);
    MacPrefsFinishSession(0);
}

void MacPrefsBeginSession() {
    if (!gGlobalPrefs || !gGlobalPrefs->sessionData) {
        return;
    }
    for (SessionData* data : *gGlobalPrefs->sessionData) {
        FreeStruct(&gSessionDataInfo, data);
    }
    VecReset(*gGlobalPrefs->sessionData);
    gBuildingSession = nullptr;
    if (!gGlobalPrefs->rememberOpenedFiles) {
        return;
    }
    gBuildingSession = (SessionData*)DeserializeStruct(&gSessionDataInfo, {});
    VecAppend(*gGlobalPrefs->sessionData, gBuildingSession);
}

void MacPrefsAppendSession(const char* path, const MacPrefsViewState* state) {
    if (!gBuildingSession || !path || !state) {
        return;
    }
    MacPrefsSaveDocument(path, state);
    FileState* fileState = FindFileState(Str((char*)path));
    if (!fileState) {
        return;
    }
    auto* tab = (TabState*)DeserializeStruct(&gTabStateInfo, {});
    str::ReplaceWithCopy(&tab->filePath, fileState->filePath);
    str::ReplaceWithCopy(&tab->displayMode, fileState->displayMode);
    str::ReplaceWithCopy(&tab->pageNo, fileState->pageNo);
    str::ReplaceWithCopy(&tab->zoom, fileState->zoom);
    tab->rotation = fileState->rotation;
    VecAppend(*gBuildingSession->tabStates, tab);
}

void MacPrefsFinishSession(int activeTab) {
    if (!gBuildingSession) {
        return;
    }
    if (len(*gBuildingSession->tabStates) == 0) {
        VecRemove(*gGlobalPrefs->sessionData, gBuildingSession);
        FreeStruct(&gSessionDataInfo, gBuildingSession);
    } else {
        gBuildingSession->tabIndex = limitValue(activeTab + 1, 1, len(*gBuildingSession->tabStates));
    }
    gBuildingSession = nullptr;
}

char* MacPrefsCopySessionPath(MacPrefsViewState* state) {
    return MacPrefsCopySessionTab(0, state);
}

int MacPrefsSessionCount() {
    SessionData* session = SavedSession();
    return session && session->tabStates ? len(*session->tabStates) : 0;
}

char* MacPrefsCopySessionTab(int index, MacPrefsViewState* state) {
    SessionData* session = SavedSession();
    if (!session || !session->tabStates || index < 0 || index >= len(*session->tabStates)) {
        return nullptr;
    }
    TabState* tab = (*session->tabStates)[index];
    if (state) {
        *state = {};
        state->valid = true;
        state->continuous = IsContinuous(DisplayModeFromString(tab->displayMode, DisplayMode::Continuous));
        state->zoomVirtual = ZoomFromString(tab->zoom, kZoomFitWidth);
        state->rotation = tab->rotation;
        state->pageNo = PageNoFromStored(tab->pageNo);
    }
    return CopyCString(tab->filePath);
}

int MacPrefsSessionActiveTab() {
    SessionData* session = SavedSession();
    return session ? session->tabIndex - 1 : 0;
}

int MacPrefsRecentCount() {
    if (!gGlobalPrefs || !gGlobalPrefs->fileStates) {
        return 0;
    }
    int count = 0;
    for (FileState* state : *gGlobalPrefs->fileStates) {
        if (!state->isMissing && len(state->filePath) > 0 && ++count == 10) {
            break;
        }
    }
    return count;
}

char* MacPrefsCopyRecentPath(int index) {
    if (index < 0) {
        return nullptr;
    }
    for (FileState* state : *gGlobalPrefs->fileStates) {
        if (state->isMissing || len(state->filePath) == 0) {
            continue;
        }
        if (index-- == 0) {
            return CopyCString(state->filePath);
        }
    }
    return nullptr;
}

static Favorite* FindFavorite(FileState* state, int pageNo) {
    if (!state || !state->favorites) {
        return nullptr;
    }
    for (Favorite* favorite : *state->favorites) {
        if (!favorite->isTemporary && PageNoFromStored(favorite->pageNo) == pageNo) {
            return favorite;
        }
    }
    return nullptr;
}

bool MacPrefsAddFavorite(const char* path, int pageNo) {
    if (!gGlobalPrefs || !path || pageNo < 1) {
        return false;
    }
    Str filePath((char*)path);
    FileState* state = FindFileState(filePath);
    if (!state) {
        state = NewFileState(filePath);
        MoveFileStateToFront(state);
    }
    if (FindFavorite(state, pageNo)) {
        return false;
    }
    VecAppend(*state->favorites, NewFavorite(pageNo));
    return true;
}

bool MacPrefsRemoveFavorite(const char* path, int pageNo) {
    FileState* state = path ? FindFileState(Str((char*)path)) : nullptr;
    Favorite* favorite = FindFavorite(state, pageNo);
    if (!favorite) {
        return false;
    }
    VecRemove(*state->favorites, favorite);
    FreeStruct(&gFavoriteInfo, favorite);
    return true;
}

bool MacPrefsHasFavorite(const char* path, int pageNo) {
    return path && FindFavorite(FindFileState(Str((char*)path)), pageNo);
}

static Favorite* FavoriteAt(int index, FileState** stateOut) {
    if (!gGlobalPrefs || !gGlobalPrefs->fileStates || index < 0) {
        return nullptr;
    }
    for (FileState* state : *gGlobalPrefs->fileStates) {
        if (!state->favorites) {
            continue;
        }
        for (Favorite* favorite : *state->favorites) {
            if (favorite->isTemporary) {
                continue;
            }
            if (index-- == 0) {
                *stateOut = state;
                return favorite;
            }
        }
    }
    return nullptr;
}

int MacPrefsFavoriteCount() {
    int count = 0;
    if (!gGlobalPrefs || !gGlobalPrefs->fileStates) {
        return count;
    }
    for (FileState* state : *gGlobalPrefs->fileStates) {
        if (state->favorites) {
            for (Favorite* favorite : *state->favorites) {
                count += !favorite->isTemporary;
            }
        }
    }
    return count;
}

char* MacPrefsCopyFavoritePath(int index) {
    FileState* state = nullptr;
    return FavoriteAt(index, &state) && state ? CopyCString(state->filePath) : nullptr;
}

int MacPrefsFavoritePage(int index) {
    FileState* state = nullptr;
    Favorite* favorite = FavoriteAt(index, &state);
    return favorite ? PageNoFromStored(favorite->pageNo) : 0;
}
