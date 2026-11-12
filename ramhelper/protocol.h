// the host app project
// Copyright (C) 2026 the host app team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// RAM helpers: the wire format between the host app and its helper extension (RamHelper.appex).
//
// iOS gives every process its own memory limit, app extensions included. A helper is a copy of the
// extension running as its own process: it allocates purgeable memory, which iOS charges to the
// process that owns it, and hands it to the host app as shared memory. The host app can map it over part of the
// guest RAM, so that memory no longer counts against the app's own limit. Several helpers
// can run at once, each with its own limit. The technique is the "RAM donor" of MeloNX (Stossy11,
// MIT licence); LiveContainer 3.8.13+ runs these extensions for apps inside it too.

#pragma once

// Memory moves in chunks of this size
#define RAMH_HELPER_CHUNK (256ull * 1024ull * 1024ull)

// Principal class of the extension; the host app finds the extension in PlugIns by it
#define RAMH_HELPER_PRINCIPAL "RamHelperHandler"

// Message keys
#define RAMH_KEY_OP "op"
#define RAMH_KEY_SIZE "size"
#define RAMH_KEY_SHMEM "shmem"
#define RAMH_KEY_ERROR "error"
#define RAMH_KEY_AVAILABLE "available"
#define RAMH_KEY_FOOTPRINT "footprint"
#define RAMH_KEY_ID "id" // region id: returned by create, passed to release

// Operations
#define RAMH_OP_HELLO "hello" // helper -> the host app, once connected
#define RAMH_OP_CREATE "create" // a region of RAMH_KEY_SIZE bytes, returned as RAMH_KEY_SHMEM
#define RAMH_OP_RELEASE "release" // drop the region RAMH_KEY_ID, or every region when no id is given
#define RAMH_OP_AVAILABLE "available" // os_proc_available_memory() of the helper
#define RAMH_OP_FOOTPRINT "footprint" // phys_footprint of the helper
