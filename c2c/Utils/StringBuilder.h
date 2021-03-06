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

#ifndef UTILS_STRINGBUILDER_H
#define UTILS_STRINGBUILDER_H

#include <string>
#include <sys/types.h>
#include <inttypes.h>

namespace C2 {

class StringBuilder {
public:
    StringBuilder(unsigned cap = CAPACITY, char* buf = 0);
    ~StringBuilder();

    StringBuilder& operator<<(const char* input);
    StringBuilder& operator<<(void* input);
    StringBuilder& operator<<(const std::string& input);
    StringBuilder& operator<<(char input);
    StringBuilder& operator<<(int32_t input);
    StringBuilder& operator<<(u_int32_t input);
    StringBuilder& operator<<(int64_t input);
    StringBuilder& operator<<(u_int64_t input);
    StringBuilder& operator<<(const StringBuilder& input);

    void radix(unsigned radix, int64_t value);

    operator const char*() const;
    void clear();
    unsigned size() const;
    unsigned space_left() const;
    bool isEmpty() const;
    void strip(char c);
    void indent(unsigned num);
    void setColor(const char* color);
    void enableColor(bool enable) { colors = enable; }
private:
    static const unsigned CAPACITY = 1024*1024;
    unsigned capacity;
    char* buffer;
    char* ptr;
    bool ownBuf;
    bool colors;

    StringBuilder(const StringBuilder&);
    StringBuilder& operator= (const StringBuilder&);
};

}

#endif

