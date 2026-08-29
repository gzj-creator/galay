Galay vendors simdjson v4.6.9 from the upstream single-header release.
The copyright and license inventory below is retained from the Debian package
metadata and covers the amalgamated header and implementation, including the
embedded third-party components.  Galay's GCC 16 linkage adaptation is
described in README.md and does not change the upstream licensing terms.

Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Source: https://github.com/lemire/simdjson

Files: *
Copyright: 2019-2020 Daniel Lemire
           2019-2020 Geoff Langdale
License: Apache-2.0
Comment: File singleheader/simdjson.cpp contains some bits from
 src/to_chars.cpp (Copyright 2009 Florian Loitsch, MIT/Expat License)

Files: dependencies/jsoncppdist/json/json-forwards.h
       dependencies/jsoncppdist/json/json.h
       dependencies/jsoncppdist/jsoncpp.cpp
Copyright: 2007-2010, Baptiste Lepilleur and
License: Expat

Files: benchmark/linux/linux-perf-events.h
Copyright: 2005-2016, Wojciech Muła
License: BSD-2-Clause
Comment: https://github.com/WojciechMula/toys/blob/master/LICENSE

Files: fuzz/*
Copyright: 2020, Paul Dreik
License: Apache-2.0

Files: src/internal/isadetection.h
Copyright: Copyright (c) 2016-     Facebook, Inc            (Adam Paszke)
           Copyright (c) 2014-     Facebook, Inc            (Soumith Chintala)
           Copyright (c) 2011-2014 Idiap Research Institute (Ronan Collobert)
           Copyright (c) 2012-2014 Deepmind Technologies    (Koray Kavukcuoglu)
           Copyright (c) 2011-2012 NEC Laboratories America (Koray Kavukcuoglu)
           Copyright (c) 2011-2013 NYU                      (Clement Farabet)
           Copyright (c) 2006-2010 NEC Laboratories America (Ronan Collobert, Leon Bottou,
           Iain Melvin, Jason Weston) Copyright (c) 2006      Idiap Research Institute
           (Samy Bengio) Copyright (c) 2001-2004 Idiap Research Institute (Ronan Collobert,
           Samy Bengio, Johnny Mariethoz)
           2019-2020 Daniel Lemire
           2019-2020 Geoff Langdale
License: BSD-3-Clause
Comment: this file has been significantly modified after copying from source.

Files: include/simdjson/nonstd/string_view.hpp
Copyright: 2017-2019, Martin Moene
License: BSL-1.0

Files: src/to_chars.cpp
Copyright: 2009, Florian Loitsch.
License: Expat

Files: windows/toni_ronnko_dirent.h
Copyright: 1998-2019, Toni Ronkko
License: Expat
Comment: https://github.com/tronkko/dirent

Files: windows/getopt.h
Copyright: 2002, Todd C. Miller <Todd.Miller@courtesan.com>
           2000 The NetBSD Foundation, Inc.
License: BSD-2-clause AND ISC

Files: debian/*
Copyright: 2019-2022, Mo Zhou <lumin@debian.org>
License: Apache-2.0

License: Expat
 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:
 .
 The above copyright notice and this permission notice shall be included
 in all copies or substantial portions of the Software.
 .
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

License: BSD-3-Clause
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:
 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
 3. Neither the name of the University nor the names of its contributors
    may be used to endorse or promote products derived from this software
    without specific prior written permission.
 .
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE HOLDERS OR
 CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

License: BSD-2-Clause
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:
 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
 .
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE HOLDERS OR
 CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

License: Apache-2.0
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at
 .
 http://www.apache.org/licenses/LICENSE-2.0
 .
 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 .
 On Debian systems, the complete text of the Apache version 2.0 license
 can be found in "/usr/share/common-licenses/Apache-2.0".

License: BSL-1.0
 Permission is hereby granted, free of charge, to any person or organization
 obtaining a copy of the software and accompanying documentation covered by this
 license (the "Software") to use, reproduce, display, distribute, execute, and
 transmit the Software, and to prepare derivative works of the Software, and to
 permit third-parties to whom the Software is furnished to do so, all subject to
 the following:
 .
 The copyright notices in the Software and this entire statement, including the
 above license grant, this restriction and the following disclaimer, must be
 included in all copies of the Software, in whole or in part, and all derivative
 works of the Software, unless such copies or derivative works are solely in the
 form of machine-executable object code generated by a source language
 processor.
 .
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE FOR ANY
 DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE, ARISING
 FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.

License: ISC
 Permission to use, copy, modify, and/or distribute this software for any
 purpose with or without fee is hereby granted, provided that the above
 copyright notice and this permission notice appear in all copies.
 .
 THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 PERFORMANCE OF THIS SOFTWARE.
