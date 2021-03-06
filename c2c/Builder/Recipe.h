/* Copyright 2013,2014,2015 Bas van den Berg
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef BUILDER_RECIPE_H
#define BUILDER_RECIPE_H

#include <string>
#include <vector>

#include "Utils/GenUtils.h"

namespace C2 {

class Recipe {
public:
    Recipe(const std::string& name_, GenUtils::TargetType type_)
        : name(name_)
        , type(type_)
        , generateDeps(false)
        , generateIR(false)
        , generateCCode(false)
    {}
    ~Recipe() {}

    void addFile(const std::string& name_);
    void addConfig(const std::string& config_);
    void addExported(const std::string& mod_);
    void addAnsiCConfig(const std::string& config_);
    void addCodeGenConfig(const std::string& config_);
    void addDepsConfig(const std::string& config_);
    void silenceWarning(const std::string& warn_);

    int size() const { return files.size(); }
    const std::string& get(int i) const;
    bool hasExported(const std::string& mod) const;

    std::string name;
    GenUtils::TargetType type;

    bool generateDeps;
    bool generateIR;
    bool generateCCode;

    typedef std::vector<std::string> Files;
    Files files;

    typedef std::vector<std::string> StringList;

    StringList configs;
    StringList exported;
    StringList cConfigs;
    StringList genConfigs;
    StringList depConfigs;
    StringList silentWarnings;
private:
    Recipe(const Recipe&);
    Recipe& operator= (const Recipe&);
};

}

#endif

