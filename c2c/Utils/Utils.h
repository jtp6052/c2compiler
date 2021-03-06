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

#ifndef UTILS_UTILS_H
#define UTILS_UTILS_H

#include <stdint.h>
#include <sys/types.h>

namespace C2 {

class Utils {
public:
    static u_int64_t getCurrentTime();
    static u_int64_t bitmask(unsigned width) {
        // create mask, 4 -> 0xF,  5-> 0x1F, etc
        uint64_t mask = 0;
        for (unsigned i=0; i<width; i++) {
            mask |= (1lu<<i);
        }
        return mask;
    }
};

}

#endif

