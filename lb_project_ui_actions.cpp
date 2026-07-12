#include "lb_project_ui_actions.h"

#include <wx/dir.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/string.h>
#include <wx/utils.h>
#include <wx/window.h>

#include "project_manager.h"

// Open an arbitrary filesystem path in the OS's default handler.
// |friendlyName| is used in error message text only ("Could not
// open the project Sources folder."). Returns true on success.
bool LbLaunchPathInOS(wxWindow* parent,
                      const std::string& path,
                      const std::string& friendlyName)
{
#ifdef __WXMSW__
    wxString cmd = "explorer.exe \"" + wxString::FromUTF8(path) + "\"";
    if (wxExecute(cmd, wxEXEC_ASYNC) != 0) return true;
#else
    if (wxLaunchDefaultApplication(wxString::FromUTF8(path))) return true;
#endif

    wxString msg = "Could not open the " + wxString::FromUTF8(friendlyName) + ".";
    wxMessageBox(msg, "Projects", wxOK | wxICON_ERROR, parent);
    return false;
}

void LbOpenProjectFolderByRoot(wxWindow* parent, const std::string& root)
{
    if (root.empty()) return;

    if (!wxDirExists(wxString::FromUTF8(root))) {
        wxString msg = "The project folder no longer exists:\n\n";
        msg += wxString::FromUTF8(root);
        wxMessageBox(msg, "Project Folder Missing",
                     wxOK | wxICON_WARNING, parent);
        return;
    }

    LbLaunchPathInOS(parent, root, "project folder");
}

void LbOpenProjectInstructionsByRoot(wxWindow* parent, const std::string& root)
{
    if (root.empty()) return;

    const std::string path = ProjectManager::ProjectInstructionsPath(root);
    if (!wxFileExists(wxString::FromUTF8(path))) {
        wxString msg = "PROJECT.md was not found:\n\n";
        msg += wxString::FromUTF8(path);
        wxMessageBox(msg, "Projects", wxOK | wxICON_WARNING, parent);
        return;
    }

    if (!wxLaunchDefaultApplication(wxString::FromUTF8(path))) {
        wxMessageBox("Could not open PROJECT.md.",
                     "Projects", wxOK | wxICON_ERROR, parent);
    }
}

void LbOpenProjectSourcesFolderByRoot(wxWindow* parent, const std::string& root)
{
    if (root.empty()) return;

    const std::string sources = ProjectManager::ProjectSourcesPath(root);
    bool ok = wxFileName::Mkdir(wxString::FromUTF8(sources),
                                wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    if (!ok && !wxDirExists(wxString::FromUTF8(sources))) {
        wxString msg = "Could not open or create the project Sources folder:\n\n";
        msg += wxString::FromUTF8(sources);
        wxMessageBox(msg, "Projects", wxOK | wxICON_ERROR, parent);
        return;
    }

    LbLaunchPathInOS(parent, sources, "project Sources folder");
}

void LbOpenProjectWorkflowsFolderByRoot(wxWindow* parent, const std::string& root)
{
    if (root.empty()) return;

    const std::string workflows = ProjectManager::ProjectWorkflowsPath(root);
    bool ok = wxFileName::Mkdir(wxString::FromUTF8(workflows),
                                wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    if (!ok && !wxDirExists(wxString::FromUTF8(workflows))) {
        wxString msg = "Could not open or create the project Workflows folder:\n\n";
        msg += wxString::FromUTF8(workflows);
        wxMessageBox(msg, "Projects", wxOK | wxICON_ERROR, parent);
        return;
    }

    LbLaunchPathInOS(parent, workflows, "project Workflows folder");
}
