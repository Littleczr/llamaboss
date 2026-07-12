#pragma once

#include <string>

class wxWindow;

bool LbLaunchPathInOS(wxWindow* parent,
                      const std::string& path,
                      const std::string& friendlyName);

void LbOpenProjectFolderByRoot(wxWindow* parent, const std::string& root);
void LbOpenProjectInstructionsByRoot(wxWindow* parent, const std::string& root);
void LbOpenProjectSourcesFolderByRoot(wxWindow* parent, const std::string& root);
void LbOpenProjectWorkflowsFolderByRoot(wxWindow* parent, const std::string& root);
