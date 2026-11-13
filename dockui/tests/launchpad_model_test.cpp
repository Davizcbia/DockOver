// Headless tests for the Launchpad layout model and the app-registry helpers.
// Usage: LiquidLaunchpadModelTest.exe [--discover]   (--discover also enumerates the real AppsFolder, read-only)
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <string>
#include <vector>

#include "app_registry.h"
#include "launchpad_model.h"

using namespace dock::launchpad;

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

static std::vector<std::string> Ids(const Layout& l) {
    std::vector<std::string> out;
    for (const Node& n : l.nodes) out.push_back(n.Key());
    return out;
}

int wmain(int argc, wchar_t** argv) {
    const std::vector<std::string> apps = {"app:a", "app:b", "app:c", "app:d", "app:e"};
    Layout l = DefaultLayout(apps);
    CHECK(l.nodes.size() == 5);

    // Reorder.
    CHECK(MoveNode(l, 0, 3));
    CHECK((Ids(l) == std::vector<std::string>{"app:b", "app:c", "app:d", "app:a", "app:e"}));
    CHECK(!MoveNode(l, 2, 2));
    CHECK(MoveNode(l, 4, 0));
    CHECK(Ids(l)[0] == "app:e");

    // Folder creation: drag app:c (index 2) onto app:b (index 1).
    l = DefaultLayout(apps);
    int f = DropOnto(l, 2, 1, L"Folder", "f1");
    CHECK(f == 1);
    CHECK(l.nodes.size() == 4);
    CHECK(l.nodes[1].folder && l.nodes[1].apps.size() == 2 && l.nodes[1].apps[0] == "app:b" && l.nodes[1].apps[1] == "app:c");
    // Add another app (app:a at index 0) into the folder (now index 1).
    f = DropOnto(l, 0, 1, L"ignored", "f2");
    CHECK(f == 0);
    CHECK(l.nodes[0].folder && l.nodes[0].apps.size() == 3 && l.nodes[0].folderId == "f1");
    // Folders cannot be dropped onto apps.
    CHECK(DropOnto(l, 0, 1, L"x", "f3") == -1);
    // Reorder inside the folder.
    CHECK(MoveWithinFolder(l, 0, 2, 0));
    CHECK(l.nodes[0].apps[0] == "app:a");
    // Rename.
    CHECK(RenameFolder(l, 0, L"  Tools  "));
    CHECK(l.nodes[0].folderName == L"Tools");
    CHECK(!RenameFolder(l, 0, L"   "));
    // Move out; folder keeps two apps.
    CHECK(MoveOutOfFolder(l, 0, 0, 5));
    CHECK(l.nodes[0].folder && l.nodes[0].apps.size() == 2);
    CHECK(l.nodes.back().appId == "app:a");
    // Move out again: folder dissolves into its last app at its position.
    CHECK(MoveOutOfFolder(l, 0, 0, 0));
    CHECK(!l.nodes[0].folder);
    for (const Node& n : l.nodes) CHECK(!n.folder);
    CHECK(l.nodes.size() == 5);

    // Merge: uninstalled apps dropped (also from folders), new apps appended, folder order kept.
    Layout saved;
    {
        Node fo;
        fo.folder = true;
        fo.folderId = "f9";
        fo.folderName = L"Work";
        fo.apps = {"app:x", "app:b", "app:c"};
        Node e;
        e.appId = "app:e";
        Node gone;
        gone.appId = "app:gone";
        saved.nodes = {e, fo, gone};
    }
    Layout merged = Merge(saved, {"app:a", "app:b", "app:c", "app:e", "app:new"});
    CHECK((Ids(merged) == std::vector<std::string>{"app:e", "folder:f9", "app:a", "app:new"}));
    CHECK(merged.nodes[1].apps.size() == 2);
    // A folder reduced to one app is dissolved.
    Layout merged2 = Merge(saved, {"app:b", "app:e"});
    CHECK((Ids(merged2) == std::vector<std::string>{"app:e", "app:b"}));
    // Duplicates are removed.
    Layout dup;
    dup.nodes = {saved.nodes[0], saved.nodes[0]};
    Normalize(dup);
    CHECK(dup.nodes.size() == 1);

    // JSON round trip, including non-ASCII folder names; invalid input is rejected atomically.
    RenameFolder(merged, 1, L"工作 Tools");
    Layout back;
    std::wstring err;
    CHECK(FromJson(ToJson(merged), back, err));
    CHECK(Ids(back) == Ids(merged));
    CHECK(back.nodes[1].folderName == L"工作 Tools");
    Layout untouched = back;
    CHECK(!FromJson("{\"nodes\": [ {\"app\": 3} ]}", untouched, err));
    CHECK(Ids(untouched) == Ids(back));
    CHECK(!FromJson("{\"nodes\": [", untouched, err));

    // Save/Load through a temporary file.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"liquid-launchpad-model-test.json";
    CHECK(Save(path, merged, err));
    Layout loaded;
    bool exists = false;
    CHECK(Load(path, loaded, err, &exists) && exists);
    CHECK(Ids(loaded) == Ids(merged));
    DeleteFileW(path.c_str());
    CHECK(Load(path, loaded, err, &exists) && !exists);

    // Registry helpers.
    using dock::apps::IsLaunchableEntry;
    using dock::apps::MatchesQuery;
    CHECK(IsLaunchableEntry(L"Notepad", L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App"));
    CHECK(!IsLaunchableEntry(L"Uninstall Foo", L"C:\\Foo\\unins000.exe"));
    CHECK(!IsLaunchableEntry(L"卸载 Foo", L"C:\\Foo\\uninst.exe"));
    CHECK(!IsLaunchableEntry(L"Foo Website", L"C:\\Foo\\site.url"));
    CHECK(!IsLaunchableEntry(L"Read me", L"C:\\Foo\\readme.txt"));
    CHECK(MatchesQuery(L"Microsoft Edge", L"edge"));
    CHECK(MatchesQuery(L"Microsoft Edge", L""));
    CHECK(!MatchesQuery(L"Microsoft Edge", L"word"));
    CHECK(dock::apps::NameLess(L"app2", L"app10"));

    if (argc > 1 && wcscmp(argv[1], L"--discover") == 0) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::vector<dock::apps::AppEntry> found;
        std::wstring derr;
        LARGE_INTEGER t0, t1, fq;
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t0);
        const bool ok = dock::apps::DiscoverApps(found, derr);
        QueryPerformanceCounter(&t1);
        CHECK(ok);
        CHECK(found.size() > 5);
        std::printf("discover: %zu apps in %.0f ms\n", found.size(), (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart);
        CoUninitialize();
    }

    {   // hidden apps (MyDockFinder "Replace everything"): kept out of the grid, survive JSON, new installs still appear
        Layout saved;
        Node a; a.appId = "app:a"; saved.nodes.push_back(a);
        saved.hidden = {"app:b", "app:gone"};
        Layout back; std::wstring err;
        CHECK(FromJson(ToJson(saved), back, err));
        CHECK(back.hidden.size() == 2 && back.hidden[0] == "app:b");
        const Layout m = Merge(back, {"app:a", "app:b", "app:c"});
        CHECK(m.nodes.size() == 2 && m.nodes[0].appId == "app:a" && m.nodes[1].appId == "app:c");
        CHECK(m.hidden.size() == 1 && m.hidden[0] == "app:b");  // uninstalled hidden ids are dropped
    }

    if (g_failures) std::printf("launchpad model: %d FAILED\n", g_failures);
    else std::printf("launchpad model: all checks passed\n");
    return g_failures ? 1 : 0;
}
