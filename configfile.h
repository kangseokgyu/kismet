/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef __CONFIGFILE_H__
#define __CONFIGFILE_H__

#include "config.h"

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <pwd.h>

#include <string>
#include <map>
#include <vector>

// Munge a string to characters safe for calling in a shell
void MungeToShell(char *in_data, int max);
string MungeToShell(string in_data);

string StrLower(string in_str);
string StrStrip(string in_str);

int XtoI(char x);
int Hex2UChar13(unsigned char *in_hex, unsigned char *in_chr);

class ConfigFile {
public:

    int ParseConfig(const char *in_fname);
    string FetchOpt(string in_key);
    vector<string> FetchOptVec(string in_key);

    static string ExpandLogPath(string path, string logname, string type, int start, int overwrite = 0);

protected:
    map<string, vector<string> > config_map;
};

#endif

