// SPDX-License-Identifier: AGPL-3.0-or-later
// Copies of the saves, made before the game writes over one (port/platform/save_backup.cpp).
#ifndef PORT_SAVE_BACKUP_H
#define PORT_SAVE_BACKUP_H

#include <string>

namespace port {

// The file at host (the card's name for it: name) is about to be written: copied, if it is there
// and not empty, into root's sibling folder root + "-backups":
//   first/      the first copy ever made of each save, never let go;
//   sessions/   for each run of the program, the copy of each save from before the run first
//               wrote it (the save as the player left it), the last 20 runs kept;
//   recent/     every copy, the last 60 kept.
// A copy the same as the last one made of that save is not made again. Returns whether a copy was
// made (false also when the file is not there or cannot be read).
bool BackUpSave(const std::string& root, const std::string& host, const std::string& name);

// The backups' folder for a save folder.
std::string BackupFolder(const std::string& root);

}  // namespace port

#endif
