/* How many MeshCore group channels this port compiles in.
 *
 * MeshCore's channel support is guarded by MAX_GROUP_CHANNELS throughout
 * BaseChatMesh: the table itself (BaseChatMesh.h:69-71), its constructor's
 * memset (:86), the declaration of searchChannelsByHash (:142) and the four
 * table accessors (BaseChatMesh.cpp:367, :877). Leaving the macro undefined
 * is upstream's own way of compiling channels out, and this library did
 * exactly that until channels were wanted; defining it here is what turns
 * them on.
 *
 * ---- why this lives in a header, and why Arduino.h includes it ----------
 *
 * The macro decides a CLASS LAYOUT:
 *
 *     ChannelDetails channels[MAX_GROUP_CHANNELS];   // BaseChatMesh.h:70
 *
 * so every translation unit that can see `class BaseChatMesh` must agree on
 * it. Two consumers compile against that class from different makefiles -
 * BaseChatMesh.cpp inside libmeshcore.a, and services/meshcored's
 * mesh_runtime.cpp - and if they were given the value through -D in two
 * places, a change to one would produce a library and a service that
 * disagree about where every member after `channels` lives. That is a
 * one-definition-rule violation the linker cannot see: the build succeeds and
 * the service writes a contact into the middle of a channel key.
 *
 * So it is defined once, in a header, and reached the one way upstream
 * guarantees. Both headers that expand MAX_GROUP_CHANNELS include <Arduino.h>
 * before they do - BaseChatMesh.h:3 and ChannelDetails.h:3 - and <Arduino.h>
 * on this platform is compat/Arduino.h, which includes this file. There is
 * therefore no include order in which a translation unit can see the class
 * without first seeing this value, and no way to compile half the tree with a
 * different one.
 *
 * ---- why eight ---------------------------------------------------------
 *
 * The cost of a slot is 65 bytes and one byte comparison per received group
 * frame, so the number is not a memory decision. Eight is what the service
 * above this library is prepared to hold, persist and present, and a limit a
 * reader can see all of at once. Upstream's own RIFT firmware uses 40; this
 * can grow to it without anything here changing but the number, because
 * nothing in the port, the service or the app hard-codes eight anywhere else
 * - services/meshcored/mesh_runtime.cpp static_asserts its own copy against
 * this one rather than repeating it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifndef MAX_GROUP_CHANNELS
#define MAX_GROUP_CHANNELS 8
#endif
