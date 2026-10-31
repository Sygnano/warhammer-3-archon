/* archon_native.dll - Archon Mode native patches, v1.0.0.
 *   A/B/C: lobby patch set.
 *   D (0.4.0): "partner leave" hook.
 *   E (0.5.0): resumed-lobby seat picker hook.
 *   0.6.0: cleanup only (patch logic unchanged): the read-only resume_info() export and its lobby-chain
 *   resolution, and the unused version()/log() exports were removed. Lua exports: status, patch_lobby,
 *   unpatch_lobby, patch_leave, unpatch_leave, patch_resume, unpatch_resume. The Lua side applies A+B+C, D and E
 *   whenever this DLL is loaded and supported (no flag file any more).
 *   F (0.7.0): end-turn consensus on a shared faction.
 *   G (0.7.1): the battle gift system on a shared army.
 *
 * Patch G "gift": a gift is a vanilla message that edits per-player unit lists, and both Archon players start with
 * every unit. The UI assumes one original owner per army, the first slot, so only that slot keeps a card for a unit
 * it gave away. G retargets eight `call`s to answer per unit: the slot that lost a unit keeps a greyed card on any
 * machine, the recall button makes the unit shared again, and the popup gets a row for the local player, which takes
 * the selection from the others. Nothing writes model state; the only effect on the network is vanilla gift messages.
 *
 * Patch F "end turn": local UI only, the model and the command executors are never touched. Lua applies it only on
 * a machine whose local faction is shared. Two 16-byte jmp hooks on the local issuers:
 *   issuer(env, flag)  0x141e6ce14 (enqueues CCQ_END_TURN): swallowed, the press is recorded in g_et_view;
 *   cancel(env)        0x141e65e50 (enqueues CCQ_CANCEL_END_TURN): swallowed, the press is withdrawn.
 * Lua polls the press and votes over the synced UITrigger channel; the machine that completes the vote set calls
 * endturn_issue(), which runs the CampaignRoot.EndTurn executor (no arguments, applies the vanilla gates) with the
 * hooks in bypass, so exactly one vanilla CCQ_END_TURN enters the stream. flag=1 is the forced "End turn anyway"
 * path (0x141e87efc) and always passes.
 * Five `call find` sites that read the engine's ready set for the UI are retargeted to a near thunk that answers
 * true for the pressed local faction, so the vanilla toggle state, spinner, diplomacy tooltip, click-again-cancels
 * and the two vanilla ready side effects all follow the local press. find (0x14052b770) only clobbers rax, rcx, r8,
 * and its callers keep r9-r11 live across the call: the thunk is hand-written machine code touching rax only,
 * never a jump into C.
 *
 * Patch E "resume": when a player joins a lobby that resumes an MP save, the HOST seats him with
 * pick_saved_key(R, &account_id, &taken_keys) (0x1421b36f8; R = EMPIRE_MP+0xe0ba8, the save header's player
 * tuples: u8 count, u64 account ids, 16-byte faction keys). Vanilla returns the joiner's own saved faction only
 * if NO seated slot holds it yet, else the first saved faction nobody holds, else nothing (the joiner then keeps a
 * stale local pick). An Archon save has two tuples on one faction, so the second player of that faction is never
 * seated on it. E hooks the picker (16-byte jmp, original kept in a trampoline) and returns the joiner's own saved
 * faction while fewer slots hold it than the save has tuples for it (capacity = saved tuples per faction).
 * Otherwise the original runs. With one tuple per faction (any normal save) the rule is exactly vanilla's.
 *
 * A Lua 5.1 module DLL for Total War: WARHAMMER III 9.0.4 (build 4375593).
 * It registers C functions into the live Lua state and can apply three reversible
 * byte patches to the lobby code so two players can pick the same faction and Ready.
 *
 * Patch D "leave": when a network player leaves, the session authority (host / new host) sends the campaign
 * command CCQ_FACTION_SWITCH_HUMAN_TO_AI(<faction key>) through EMPIRE_MP::send_faction_switch_human_to_ai
 * (0x1421d1f5c, 3 callers: player_left 0x1421c95a8, HOST_MIGRATED tick reader 0x1421b7950, post-battle
 * notify_campaign_env_created 0x1421c5aac). Every machine executes it (handler 0x1428b2ea0 -> world 0x1426d9154 ->
 * faction set_idle_human 0x1426cf528: player type byte +0x1bc4 = 1 = IDLE_HUMAN) and the faction's turns are then
 * skipped. D replaces the first 16 bytes of the sender with an absolute jmp to hook_send_switch() below, which
 * calls the original (through a trampoline holding the 16 relocated, position-independent bytes) UNLESS another
 * connected network player still controls the same faction. The decision is taken on the sending machine only;
 * its outcome (command present or absent in the synchronised command stream) is identical for every machine.
 * A byte patch cannot express "another connected player has the same faction key" (a loop over the 32-slot
 * network player table plus a string compare), hence the hook; it lives in this DLL, nothing is written into
 * the exe except the 16-byte jmp, and any exception inside the decision falls back to the original behaviour.
 *
 * Design:
 *   - bind the Lua C API by name from the exe module (GetProcAddress), never bundle a second Lua core;
 *   - pin the module (GET_MODULE_HANDLE_EX_FLAG_PIN) so it survives lua_close at mode switches;
 *   - never refuse a game version (0.7.2): an untested exe is logged and runs at the player's own risk. Each patch
 *     still needs its signatures, because that is how it finds what to write; one that does not resolve stays off;
 *   - resolve every target by signature scan over NON-WRITABLE code only, with structural checks;
 *   - refuse the writable+executable anti-tamper region (.didata);
 *   - every failure is a returned error string + a log line, never a crash;
 *   - no patch is applied unless ALL THREE targets resolve uniquely (all or nothing).
 *
 * Targets (addresses on the tested build):
 *   Patch A "canset":   CanSetFaction executor 0x141f2cc8c  -> first 3 bytes = B0 01 C3 (mov al,1; ret).
 *   Patch B "readyerr": the ready-error TEXT builder tests bit 0x1c of the ready mask with
 *                       `bt rax,0x1c; jae` at 0x141f411e6 (bytes 48 0F BA E0 1C), located by the
 *                       UNIQUE lea->"mp_ready_error_faction_duplicates" (0x143818bd8) at 0x141f41208,
 *                       which sits lea-0x22 after it. Patched to F8 90 90 90 90 (clc; nop*4).
 *   Patch C "producer": the ready-mask PRODUCER (v0.2.0). Per player slot (ebx = slot index) the slot's
 *                       faction is looked up in a "seen" set; if already present,
 *                         jne 0x1421b5b70   (0F 85 95 01 00 00 at 0x1421b59d5)
 *                       goes to the duplicate block (bts rax,0x1c on the mask + append the player to the
 *                       "Players preventing ready up" list). The rel32 is rewritten so the jne targets the
 *                       loop's existing "continue" path 0x1421b5b89 (inc ebx; jmp loop_head) instead:
 *                         0F 85 AE 01 00 00. Duplicate slots then set no bit and append nothing.
 *                       Located by a wildcarded signature (unique across all non-writable code sections)
 *                       plus structural checks (see resolve_producer).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Lua 5.1 headers, vendored under native/lua (MIT, luaconf.h patched LUA_NUMBER=float). We include them
 * for the correct types/macros only; the API itself is bound by GetProcAddress (see lua_api below). */
#include "lua/lua.h"
#include "lua/lauxlib.h"

/* ---- the build this was tested on: reported, not enforced ------------------------------------------- */
#define ARCHON_VERSION          "1.0.0"
#define TESTED_TIMESTAMP        0x6ACEC6F4u
#define TESTED_SIZEOFIMAGE      0x0E029000u

/* reference VAs for logging and the offline test (image base 0x140000000) */
#define REF_CANSET_VA           0x141f2cc8cull
#define REF_DUPSTRING_VA        0x143818bd8ull
#define REF_LEA_VA              0x141f41208ull
#define REF_BT_VA               0x141f411e6ull
#define REF_PRODUCER_JNE_VA     0x1421b59d5ull
#define REF_PRODUCER_CONT_VA    0x1421b5b89ull

/* ---- the duplicate-faction loc key (string anchor) ------------------------------------------------ */
static const char DUP_KEY[] = "mp_ready_error_faction_duplicates";

/* ---- Patch A bytes -------------------------------------------------------------------------------- */
static const unsigned char CANSET_NEW[3]    = { 0xB0, 0x01, 0xC3 };            /* mov al,1; ret        */
static const unsigned char CANSET_EXPECT[3] = { 0x48, 0x89, 0x5C };            /* mov [rsp+..],rbx     */
/* 30-byte unique signature of the CanSetFaction executor prologue.                                  */
static const char CANSET_SIG[] =
    "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 30 48 8B FA 48 8B E9 B3 01 48 85 D2";

/* ---- Patch B bytes -------------------------------------------------------------------------------- */
static const unsigned char BT_EXPECT[5] = { 0x48, 0x0F, 0xBA, 0xE0, 0x1C };    /* bt rax,0x1c          */
static const unsigned char BT_NEW[5]    = { 0xF8, 0x90, 0x90, 0x90, 0x90 };    /* clc; nop; nop; nop; nop */
#define LEA_TO_BT_DELTA 0x22                                                    /* bt = lea - 0x22      */

/* ---- Patch C: ready-mask producer ----------------------------------------------------------------- */
/* sig starts at 0x1421b59bc:
 *   4D 8B C7                mov r8,r15
 *   48 8D 55 70             lea rdx,[rbp+0x70]
 *   48 8D 4C 24 50          lea rcx,[rsp+0x50]
 *   E8 ????????             call <seen-set find>            (0x1421b59c8)
 *   48 8D 4C 24 58          lea rcx,[rsp+0x58]              (end sentinel)
 *   48 39 08                cmp [rax],rcx                   (found != end ?)
 *   0F 85 ????????          jne <duplicate block>           (+25, 0x1421b59d5)   <- PATCH SITE
 *   4D 8B C7                mov r8,r15                      (not found: insert into the seen set)
 *   48 8D 54 24 38          lea rdx,[rsp+0x38]
 *   48 8D 4C 24 50          lea rcx,[rsp+0x50]
 *   E8 ????????             call <seen-set insert>
 *   E9 ????????             jmp <continue>                  (+49) -> independent derivation of continue
 */
static const char PRODUCER_SIG[] =
    "4D 8B C7 48 8D 55 70 48 8D 4C 24 50 E8 ?? ?? ?? ?? 48 8D 4C 24 58 48 39 08 "
    "0F 85 ?? ?? ?? ?? 4D 8B C7 48 8D 54 24 38 48 8D 4C 24 50 E8 ?? ?? ?? ?? E9 ?? ?? ?? ??";
#define PSIG_JNE_OFF 25
#define PSIG_JMP_OFF 49
static const unsigned char BTS_RAX_1C[5] = { 0x48, 0x0F, 0xBA, 0xE8, 0x1C };   /* bts rax,0x1c         */
/* loop head: mov rax,[rsi]; mov rcx,rsi; call [rax+0x168]; cmp ebx,eax; jae ...                       */
static const unsigned char LOOPHEAD_EXPECT[16] =
    { 0x48, 0x8B, 0x06, 0x48, 0x8B, 0xCE, 0xFF, 0x90, 0x68, 0x01, 0x00, 0x00, 0x3B, 0xD8, 0x0F, 0x83 };
/* inactive-slot test inside the loop: and al,3; cmp al,1; jne <continue>                              */
static const unsigned char INACTIVE_EXPECT[6] = { 0x24, 0x03, 0x3C, 0x01, 0x0F, 0x85 };

/* ---- Patch D: partner leave ------------------------------------- */
#define REF_LEAVE_SEND_VA       0x1421d1f5cull   /* EMPIRE_MP::send_faction_switch_human_to_ai(this, key*) */
#define REF_LEAVE_HANDLER_VA    0x1428b2ea0ull   /* CCQ_FACTION_SWITCH_HUMAN_TO_AI executor               */
#define REF_LEAVE_SETIDLE_VA    0x1426cf528ull   /* faction::set_idle_human                                */
/* string anchors (.xcode); the first two are UTF-16LE log formats */
static const wchar_t PL_WSTR[]  = L"EMPIRE_MP::player_left [player_id %u] [is_local_player %u] [reason: %s]";
static const wchar_t HPL_WSTR[] = L"called handle_player_left() [player %llu] [player-id %d] [reason %d]";
static const char    CCQ_STR[]  = "CCQ_FACTION_SWITCH_HUMAN_TO_AI";
/* in player_left, after the faction-key lookup: mov rdx,[rdi+setup]; test; je; mov rdx,[rdx+sub];
 * mov rcx,rbx; call getkey; mov rdx,rax; lea rcx,[rdi-8]; call SEND; mov sil,1                         */
static const char PL_CALL_SIG[] =
    "48 8B 97 ?? ?? ?? ?? 48 85 D2 74 ?? 48 8B 52 ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B D0 48 8D 4F F8 "
    "E8 ?? ?? ?? ?? 40 B6 01";
#define PLC_SETUP_DISP 3
#define PLC_SETUP_SUB  15
#define PLC_GETKEY_E8  19
#define PLC_SEND_E8    31
/* player_left: mov rcx,[rdi+list]; mov rdx,r15; xor sil,sil; call lookup(list, &player_id) */
static const char PL_LIST_SIG[]  = "48 8B 8F ?? ?? ?? ?? 49 8B D7 40 32 F6 E8 ?? ?? ?? ??";
/* player_left: the "controls a faction" predicate on the leaving record: (flags & 0x11)==1 && (flags & 0x25)!=0x25 */
static const char PL_FLAGS_SIG[] = "8B 80 38 01 00 00 8B C8 80 E1 11 80 F9 01 75 ?? 83 E0 25 3C 25 74 ??";
/* handle_player_left: local player id = [[[this+A]+B]+C]; list = [this+LIST] */
static const char HPL_LOCAL_SIG[] = "48 8B 8B ?? ?? ?? ?? 4C 8B D8 48 8B 51 ?? 8B 8A ?? ?? ?? ??";
static const char HPL_LIST_SIG[]  = "4C 8B AB ?? ?? ?? ?? 49 8D 57 08 49 8B CD E8";
/* lookup(list, &id): id < 0x20 -> rec_at(list, [list + MAP + id*4]) */
static const char LOOKUP_SIG[] = "48 83 EC 28 83 3A 20 74 ?? 8B 02 8B 94 81 ?? ?? ?? ?? E8 ?? ?? ?? ??";
/* rec_at(list, idx): idx < [list+0x2800] && ([list + idx*0x140 + 0x138] & 1) ? record : NULL (exact bytes) */
static const char RECAT_SIG[] =
    "39 91 00 28 00 00 76 16 8B C2 48 8D 04 80 48 C1 E0 06 48 03 C1 F6 80 38 01 00 00 01 75 02 33 C0 C3";
#define NP_COUNT_OFF   0x2800u
#define NP_STRIDE      0x140u
#define NP_FLAGS_OFF   0x138u
#define NP_MAX         32u
/* getkey(rec, setup): setup players list [rec+0x90] -> entry, key string* = [entry+8] */
static const char GETKEY_SIG[] =
    "40 53 48 83 EC 20 48 8B D9 48 8B CA E8 ?? ?? ?? ?? 8B 93 90 00 00 00 48 8B C8 E8 ?? ?? ?? ?? 48 85 C0 74 06 48 8B 40 08";
/* the sender: if ([this+0x10]->vt[0x20]()) inner([this+sink], &{payload_vt, key}) */
static const char SEND_SIG[] =
    "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 48 8B FA 48 83 C1 10 48 8B 01 FF 50 20 84 C0 74 22 "
    "48 8B 8B ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 8D 54 24 20 48 89 44 24 20 48 89 7C 24 28 "
    "E8 ?? ?? ?? ?? 48 8B 5C 24 40 48 83 C4 30 5F C3";
#define SEND_PAYLOAD_LEA 37
#define SEND_INNER_E8    59
static const unsigned char SEND_PROLOGUE[16] =   /* relocated into the trampoline; position independent */
    { 0x48,0x89,0x5C,0x24,0x08, 0x57, 0x48,0x83,0xEC,0x30, 0x48,0x8B,0xD9, 0x48,0x8B,0xFA };
/* inner: reads the command id twice from the same .srdata slot */
static const char INNER_SIG[] =
    "48 89 5C 24 18 57 48 83 EC 40 8B 05 ?? ?? ?? ?? 48 8B D9 48 8D 4C 24 50 89 44 24 50 48 8B FA "
    "E8 ?? ?? ?? ?? 44 8B 05 ?? ?? ?? ??";
/* payload vtable[0]: serialise the key string */
static const char VT0_SIG[] = "48 83 EC 28 48 8B C2 48 8B 51 08 48 8B C8 E8 ?? ?? ?? ?? B0 01 48 83 C4 28 C3";
static const char SER_SIG[] = "48 89 5C 24 08 57 48 81 EC 30 01 00 00 48 8B D9 48 8B FA 48 8B CA E8 ?? ?? ?? ??";
#define SER_LEN_E8   22
#define SER_CSTR_OFF 0x3f                          /* 48 8B CF E8 <c_str> */
/* engine string accessors (exact bytes: 16-byte string, inline when tag nibble of qword+8 == 8) */
static const char STRLEN_SIG[] =
    "48 8B 41 08 49 B8 00 00 00 00 00 00 00 F0 48 8B D0 49 23 D0 49 B8 00 00 00 00 00 00 00 80 "
    "49 3B D0 74 03 8B 01 C3 48 C1 E8 38 83 E0 0F C3";
static const char CSTR_SIG[] =
    "48 8B 41 08 49 B8 00 00 00 00 00 00 00 F0 48 8B D0 49 23 D0 49 B8 00 00 00 00 00 00 00 80 "
    "49 3B D0 74 01 C3 48 8B C1 C3";
/* CCQ registration: lea r9,<handler>; xor r8d,r8d; lea rdx,"CCQ_..."; lea rcx,<id slot> */
static const unsigned char CCQ_PRE[3]  = { 0x4C, 0x8D, 0x0D };
static const unsigned char CCQ_MID[3]  = { 0x45, 0x33, 0xC0 };
static const unsigned char CCQ_POST[3] = { 0x48, 0x8D, 0x0D };
static const char HANDLER_CALL_SIG[] = "48 8B D0 48 8B CF E8 ?? ?? ?? ??";      /* world_switch(world, faction) */
static const char WORLD_CALL_SIG[]   = "48 8B CA E8 ?? ?? ?? ??";                /* set_idle_human(faction)      */
static const char SETIDLE_SIG[]      = "C6 86 C4 1B 00 00 01";                   /* mov byte [rsi+0x1bc4],1      */

/* ---- Patch E: resumed-lobby seat picker ------------------------ */
#define REF_RES_PICK_VA         0x1421b36f8ull   /* pick_saved_key(R, &account_id, &taken_keys)              */
#define REF_RES_WRAP_VA         0x1421b3d7cull   /* EMPIRE_MP: builds taken_keys from the seated slots, calls it */
#define REF_RES_JOIN_VA         0x1421cb508ull   /* host: ADD_FILLED_PLAYER_SLOT (0xC7) resumed seating        */
#define REF_RES_OFF             0xe0ba8u         /* EMPIRE_MP + this = R (resume info from the save header)    */
/* string anchors (.xcode, UTF-16LE log formats) */
static const wchar_t JOIN_WSTR[] = L"recv MP_NETWORK_LOBBY_ID_ADD_FILLED_PLAYER_SLOT about %d";
static const wchar_t SETF_WSTR[] = L"faction not set: another player already has this faction (%s)";
/* host join handler: lea rdi,[r13+R]; mov rcx,rdi; call is_resumed; test al,al; je; lea rdx,[rbp-0x18];
 * mov rcx,r13; call wrapper(mp,&account_id); mov rdx,rax; mov rcx,rdi; call index_of(R,key)             */
static const char JOIN_SIG[] =
    "49 8D BD ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8D 55 E8 49 8B CD E8 ?? ?? ?? ?? "
    "48 8B D0 48 8B CF E8 ?? ?? ?? ??";
/* wrapper: lea rcx,[rdi+R]; mov rdx,r14; lea r8,[rbp-0x20] (taken keys); call picker */
static const char WRAP_CALL_SIG[] = "48 8D 8F ?? ?? ?? ?? 49 8B D6 4C 8D 45 E0 E8 ?? ?? ?? ??";
/* the picker, whole function (212 bytes; only the three call targets are masked). Fixes the layout:
 * R+0 u8 count, R+0x10 u64* account ids, R+0x1c u32 key count, R+0x20 16-byte keys; taken {+4 count, +8 keys} */
static const char PICK_SIG[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 "
    "44 0F B6 39 33 FF 4D 8B F0 48 8B F1 45 85 FF 74 3D 4C 8B 61 10 4C 8B 2A 8B EF 4D 39 2C FC 75 27 "
    "49 8B 4E 08 41 8B 5E 04 48 C1 E3 04 48 03 D9 48 C1 E5 04 48 03 6E 20 48 8B D3 4C 8B C5 "
    "E8 ?? ?? ?? ?? 48 3B C3 74 67 FF C7 41 3B FF 72 CA 48 8B 5E 20 8B 7E 1C 48 C1 E7 04 48 03 FB "
    "48 3B DF 74 2B 41 8B 6E 04 49 8B 76 08 48 C1 E5 04 48 03 EE 4C 8B C3 48 8B D5 48 8B CE "
    "E8 ?? ?? ?? ?? 48 3B C5 74 30 48 83 C3 10 48 3B DF 75 E4 E8 ?? ?? ?? ?? "
    "48 8B 5C 24 50 48 8B 6C 24 58 48 8B 74 24 60 48 83 C4 20 41 5F 41 5E 41 5D 41 5C 5F C3 "
    "48 8B C5 EB DE 48 8B C3 EB D9";
#define PICK_FIND1_E8 89
#define PICK_FIND2_E8 149
#define PICK_EMPTY_E8 168
static const unsigned char PICK_PROLOGUE[16] =   /* relocated into the trampoline; position independent */
    { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x6C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x57 };
#define R_COUNT_U8     0x00u
#define R_IDS_PTR      0x10u
#define R_KEYCOUNT     0x1cu
#define R_KEYS_PTR     0x20u
#define TAKEN_COUNT    0x04u
#define TAKEN_PTR      0x08u
#define KEY_STRIDE     16u
/* is_resumed(R): strlen(R+0x78) != 0 || !is_empty(R+0x88) (gives the engine's strlen accessor) */
static const char RESUMED_SIG[] =
    "40 53 48 83 EC 20 4C 8B C9 48 83 C1 78 E8 ?? ?? ?? ?? 33 DB 85 C0 75 10 49 8D 89 88 00 00 00 "
    "E8 ?? ?? ?? ?? 84 C0 75 02 B3 01 8A C3 48 83 C4 20 5B C3";
/* index_of(R,key): find key in R+0x20[R+0x1c] -> R+0x30[pos] (cross-checks the key array layout) */
static const char INDEX_SIG[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 59 1C 48 8B F9 48 8B 71 20 4C 8B C2 48 C1 E3 04 "
    "48 8B CE 48 03 DE 48 8B D3 E8 ?? ?? ?? ?? 48 3B C3 74 17 48 2B C6 48 C1 F8 04 3B 47 2C 73 0B "
    "8B C8 48 8B 47 30 8B 04 88 EB 03 83 C8 FF";
/* EMPIRE_MP::set_faction (anchored by its log string): resumed branch (cross-check of R, is_resumed, index_of) */
static const char SETF_RES_SIG[]  = "48 8D 9D ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 84 C0 74 ?? 49 8B D7 48 8B CB E8 ?? ?? ?? ??";

/* ---- Patch F: end-turn consensus -------------------------- */
#define REF_ET_EXEC_VA          0x141fd8dacull   /* CampaignRoot.EndTurn executor: gates, then issuer(env, 0)    */
#define REF_ET_ISSUER_VA        0x141e6ce14ull   /* issuer(env, flag): enqueues CCQ_END_TURN                     */
#define REF_ET_CANCEL_VA        0x141e65e50ull   /* cancel(env): enqueues CCQ_CANCEL_END_TURN                    */
#define REF_ET_FIND_VA          0x14052b770ull   /* find(array, &faction) -> bool                                */
#define REF_ET_CCQ_VA           0x1428b2760ull   /* CCQ_END_TURN executor (cross-check)                          */
/* ready-set readers: getter IsFactionReadyToEndTurn, button click, button state, panel auto-cancel, predicate */
#define ET_SITES                5
static const uintptr_t REF_ET_SITES[ET_SITES] =
    { 0x142059640ull, 0x1421540ceull, 0x14215a124ull, 0x14216a578ull, 0x141e652b2ull };
/* offsets fixed by ET_ISSUER_SIG / ET_CANCEL_SIG (exact bytes) */
#define ET_OFF_UI      0x2f60u                   /* env -> campaign ui                                           */
#define ET_OFF_MODEL   0x90u
#define ET_OFF_LOCAL   0x1c8u                    /* local faction                                                */
#define ET_OFF_WORLD   0x6ca0u
#define ET_OFF_ETM     0x310u
#define ET_OFF_R       0x120u
#define ET_OFF_READY   0x10u                     /* R + this = ready set                                         */
/* string anchors (.xcode) */
static const char ET_BUTTON_STR[] = "EndTurnButton";
static const char ET_READY_STR[]  = "IsFactionReadyToEndTurn";
static const char ET_CCQ_STR[]    = "CCQ_END_TURN";
/* after lea rdx,"EndTurnButton": mov rcx,rax; call <base ctor>; lea rax,<vtable>; mov [rbx],rax */
static const char ET_FACTORY_SIG[] = "48 8B C8 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 03";
#define ET_VT_UPDATE   4                         /* EndTurnButton vtable: state updater                          */
#define ET_VT_CLICK    13                        /* EndTurnButton vtable: click handler                          */
/* the executor, whole function ("EndTurn" alone is ambiguous, it also ends two longer strings) */
static const char ET_EXEC_SIG[] =
    "40 53 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 85 DB 74 71 48 8B 83 60 2F 00 00 4C 8B 90 90 00 00 00 "
    "4C 8B 88 C8 01 00 00 49 8B 82 B0 5B 00 00 48 8B 88 A8 00 00 00 48 8B 81 70 01 00 00 8B 89 6C 01 00 00 "
    "4C 8D 04 C8 49 3B C0 74 1C 48 8B 08 48 8B 11 80 7A 10 00 75 06 48 83 C0 08 EB E9 41 80 B9 C4 1B 00 00 02 "
    "72 1C 45 33 C0 49 8B D1 49 8B CA E8 ?? ?? ?? ?? 84 C0 74 0A 33 D2 48 8B CB E8 ?? ?? ?? ?? 48 83 C4 20 5B C3";
#define ET_EXEC_CAN_E8  112
#define ET_EXEC_ISS_E8  126
static const char ET_ISSUER_SIG[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 60 48 8B D9 40 8A F2 48 8D 4C 24 38 E8 ?? ?? ?? ?? "
    "4C 8B 93 60 2F 00 00 49 8B BA 90 00 00 00 4D 8B 9A C8 01 00 00 49 8B D3 48 8B 8F A0 6C 00 00 "
    "48 8B 89 10 03 00 00 E8 ?? ?? ?? ?? 84 C0";
#define ET_ISS_TURN_E8  69
static const char ET_CANCEL_SIG[] =
    "48 83 EC 38 4C 8B 89 60 2F 00 00 49 8B 81 E0 00 00 00 80 78 4C 00 74 5C 49 8B 81 90 00 00 00 "
    "4D 8B 91 C8 01 00 00 4C 89 54 24 40 48 8B 90 A0 6C 00 00 48 8B 82 10 03 00 00 48 8D 54 24 40 "
    "48 8B 88 20 01 00 00 48 83 C1 10 E8 ?? ?? ?? ?? 84 C0 74 22 48 8D 05 ?? ?? ?? ?? 4C 89 54 24 28 "
    "48 89 44 24 20 48 8D 54 24 20 49 8B 89 98 00 00 00 E8 ?? ?? ?? ?? 48 83 C4 38 C3";
#define ET_CAN_FIND_E8  73
static const char ET_FIND_SIG[] =
    "4C 8B 41 08 8B 41 04 49 8D 0C C0 4C 3B C1 74 14 48 8B 02 49 39 00 74 09 49 83 C0 08 4C 3B C1 75 F2 "
    "4C 3B C1 0F 95 C0 C3";
#define ET_SITE_RAX_SIG  "48 8B 88 20 01 00 00 48 83 C1 10 E8 ?? ?? ?? ??"   /* click handler, state updater */
#define ET_SITE_RCX_SIG  "48 8B 89 20 01 00 00 48 83 C1 10 E8 ?? ?? ?? ??"   /* getter                       */
#define ET_SITE_E8       11
/* the two readers that first test is_factions_turn (panel auto-cancel, predicate): same code twice */
static const char ET_SITE_R11_SIG[] =
    "4C 8B 99 10 03 00 00 49 8B CB E8 ?? ?? ?? ?? 84 C0 74 ?? 49 8B 8B 20 01 00 00 48 8D 54 24 ?? "
    "48 83 C1 10 4C 89 54 24 ?? E8 ?? ?? ?? ??";
#define ET_R11_E8       40
/* click handler, after the reader: je; mov rcx,rbx; call cancel */
static const char ET_CLICK_CANCEL_SIG[] = "84 C0 74 ?? 48 8B CB E8 ?? ?? ?? ??";
#define ET_CLK_CALL_E8  7
/* CCQ_END_TURN executor: can_end_turn(model, faction, 0) -> end_turn(model, faction, force) */
static const char ET_CCQ_CALL_SIG[] =
    "45 33 C0 48 8B D3 48 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 44 8A 44 24 30 48 8B D3 48 8B CE E8 ?? ?? ?? ??";
#define ET_CCQ_CAN_E8   9
/* relocated into the trampolines; both 18-byte runs end on an instruction boundary and are position independent */
static const unsigned char ET_ISSUER_PROLOGUE[18] =
    { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x74,0x24,0x10, 0x57, 0x48,0x83,0xEC,0x60, 0x48,0x8B,0xD9 };
static const unsigned char ET_CANCEL_PROLOGUE[18] =
    { 0x48,0x83,0xEC,0x38, 0x4C,0x8B,0x89,0x60,0x2F,0x00,0x00, 0x49,0x8B,0x81,0xE0,0x00,0x00,0x00 };

/* ---- Patch G bytes --------------------------------------------------------------------------------- */
/* OriginalOwningNetworkPlayerContext, from the env read to its "first slot of this army" call */
#define GF_ORIG_SIG      "48 8B 05 ?? ?? ?? ?? 48 8B D9 48 8B 90 18 04 00 00 48 8B BA C8 45 06 00 48 85 FF 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? " \
                         "48 8B F0 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 57 18 48 8D 4F 18 FF 92 C0 01 00 00 48 85 C0 0F 84 ?? ?? ?? ?? " \
                         "48 8B 57 18 48 8D 4F 18 FF 92 C0 01 00 00 48 8B 4E 08 48 8B 91 E0 00 00 00 44 8B 81 8C 00 00 00 48 8B C8 8B 52 08 E8 ?? ?? ?? ??"
#define GF_ORIG_ARMY_E8  0x21                                       /* call army_of(context)                    */
#define GF_ORIG_FIRST_E8 0x6F                                       /* call first_slot(local, alliance, army)   */
/* IsOriginallyOwnedByLocalPlayer, whole function up to the same call */
#define GF_OWNED_SIG     "48 89 5C 24 08 57 48 83 EC 20 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8B 88 18 04 00 00 " \
                         "48 8B B9 C8 45 06 00 48 85 FF 74 ?? 48 83 C7 18 48 8B CF 48 8B 17 FF 92 C0 01 00 00 48 85 C0 74 ?? 48 8B 07 48 8B CF " \
                         "FF 90 C0 01 00 00 4C 8B 43 08 48 8B C8 49 8B 90 E0 00 00 00 45 8B 80 8C 00 00 00 8B 52 08 E8 ?? ?? ?? ??"
#define GF_OWNED_ARMY_E8 0x0A
#define GF_OWNED_FIRST_E8 0x6E
/* GiftUnits(slot): from = local slot, to = the row's slot */
#define GF_GIFT_SIG      "48 8B 07 48 8B CF FF 90 E8 00 00 00 48 8D 48 18 48 8B 01 FF 90 B8 01 00 00 48 8B C8 48 8B 10 FF 52 20 8B D0 44 8B C3 48 8B CF E8 ?? ?? ?? ??"
#define GF_GIFT_E8       0x2A
#define GF_SEL_E8        0x21                                       /* gift_selection -> gift_units             */
/* GiftToOriginalOwner: gift_units(env, owner, original, {unit}) */
#define GF_RECALL_SIG    "48 8B CE 48 89 44 24 28 48 89 18 E8 ?? ?? ?? ?? 48 8B CD 8B D8 E8 ?? ?? ?? ?? 4C 8D 4C 24 20 44 8B C3 8B D0 48 8B CF E8 ?? ?? ?? ??"
#define GF_RECALL_E8     0x27
/* gift_units: the send of tick tag 0xa9, keep flag 0 */
#define GF_SEND_SIG      "44 8B 7D 50 4C 8B 6D 48 44 8B 65 58 49 8B 8D 18 04 00 00 45 8B C4 4C 8B 0B 41 8B D7 C6 44 24 20 00 48 8B 89 C8 45 06 00 E8 ?? ?? ?? ??"
#define GF_SEND_E8       0x28
#define GF_UNITS_SPAN    0x200                                      /* that send lies this close to gift_units  */
/* the popup's player list: active humans of the alliance, then a walk that skips r15d (the local slot) */
#define GF_LIST_SIG      "8B 58 08 48 8B 06 FF 90 B8 01 00 00 48 8B C8 4C 8B 00 41 FF 50 20 4C 8D 45 E0 4C 89 75 E0 8B D3 89 45 48 49 8B CD 4C 89 75 E8 44 8B F8 E8 ?? ?? ?? ?? " \
                         "48 8B 75 E8 8B 4D E4 48 8B DE 4C 8D 24 8E 49 3B F4 74 ?? 44 8B 33 45 3B F7 74 ??"
#define GF_LIST_E8       0x2D
/* field offsets of the tested build; a game patch that moves them is the risk the player accepts */
#define GF_ENV_IFACE     0x418
#define GF_IFACE_MP      0x645c8
#define GF_MP_PLAYERS    0xe0b10
#define GF_PL_COUNT      0x2800
#define GF_SLOT_STRIDE   0x140
#define GF_SLOT_LIST_N   0x14
#define GF_SLOT_LIST_P   0x18
#define GF_SLOT_ALLIANCE 0x12c
#define GF_SLOT_ARMY     0x130
#define GF_SLOT_FLAGS    0x138
#define GF_ENTRY_STRIDE  0xf8
#define GF_ENTRY_LOW     0x98
#define GF_LOW_ID        0x3ea0
#define GF_UNIT_ID       0x614
#define GF_MAX_SLOTS     8

/* =================================================================================================== */
/* Lua C API bound by name from the exe (no second Lua core).                                          */
typedef struct {
    void (*createtable)(lua_State*, int, int);
    void (*setfield)(lua_State*, int, const char*);
    void (*pushboolean)(lua_State*, int);
    void (*pushinteger)(lua_State*, ptrdiff_t);
    void (*pushlstring)(lua_State*, const char*, size_t);
    void (*pushcclosure)(lua_State*, lua_CFunction, int);
    int  (*toboolean)(lua_State*, int);
} lua_api_t;
static lua_api_t A;

static void push_str(lua_State* L, const char* s) { A.pushlstring(L, s, strlen(s)); }
static void reg_fn(lua_State* L, const char* name, lua_CFunction fn) {
    A.pushcclosure(L, fn, 0);
    A.setfield(L, -2, name);
}

/* =================================================================================================== */
/* State                                                                                               */
static CRITICAL_SECTION g_lock;
static int   g_init_done   = 0;
static int   g_host_ok     = 0;         /* all three lobby targets resolved                            */
static int   g_exe_tested  = 0;         /* the exe is the build this version was tested on             */
static const char* g_reason = "not initialised";

static uintptr_t g_imgbase  = 0;        /* actual module base (== ImageBase, the exe has a fixed base) */
static uintptr_t g_canset   = 0;        /* resolved VA of CanSetFaction executor                      */
static uintptr_t g_bt       = 0;        /* resolved VA of `bt rax,0x1c` in the ready text builder     */
static uintptr_t g_lea      = 0;        /* resolved VA of lea->dup string                             */
static uintptr_t g_dupstr   = 0;        /* resolved VA of the dup loc key                             */
static uintptr_t g_producer = 0;        /* resolved VA of the producer `jne <dup block>`              */
static uintptr_t g_prod_dup = 0;        /* its original target (duplicate block)                      */
static uintptr_t g_prod_cont= 0;        /* the loop's continue path (inc ebx; jmp head)               */
static uintptr_t g_prod_head= 0;        /* the loop head                                              */

static int g_patch_canset   = 0;        /* 1 when patch A currently applied                           */
static int g_patch_ready    = 0;        /* 1 when patch B currently applied                           */
static int g_patch_producer = 0;        /* 1 when patch C currently applied                           */
static unsigned char g_save_canset[3];
static unsigned char g_save_ready[5];
static unsigned char g_save_producer[6];
static unsigned char g_prod_orig[6];    /* bytes read at init (0F 85 rel32_dup)                       */
static unsigned char g_prod_new[6];     /* computed (0F 85 rel32_cont)                                */

/* ---- Patch D state ---- */
typedef uint64_t    (*fn_send_t)(void* mp, void* key);
typedef void*       (*fn_getkey_t)(void* rec, void* setup);
typedef void*       (*fn_recat_t)(void* list, uint32_t idx);
typedef uint32_t    (*fn_strlen_t)(void* s);
typedef const char* (*fn_cstr_t)(void* s);
static int         g_leave_ok      = 0;   /* D targets resolved and verified                              */
static const char* g_leave_reason  = "not resolved";
static int         g_patch_leave   = 0;   /* 1 when the 16-byte jmp is installed                          */
static uintptr_t   g_leave_send    = 0, g_leave_getkey = 0, g_leave_lookup = 0, g_leave_recat = 0;
static uintptr_t   g_leave_strlen  = 0, g_leave_cstr = 0, g_leave_handler = 0, g_leave_world = 0;
static uintptr_t   g_leave_setidle = 0, g_leave_slot = 0, g_leave_pl = 0;
static uint32_t    g_off_list = 0, g_off_setup = 0, g_off_setup_sub = 0, g_off_map = 0;
static uint32_t    g_off_loc_a = 0, g_off_loc_b = 0, g_off_loc_c = 0;
static unsigned char* g_tramp      = NULL; /* VirtualAlloc'd: 16 relocated bytes + jmp [rip] back      */
static unsigned char  g_save_leave[16];
static volatile LONG  g_leave_calls = 0, g_leave_skipped = 0, g_leave_passed = 0, g_leave_errors = 0;
static CRITICAL_SECTION g_last_lock;
static char           g_leave_last[256] = "none";

/* ---- Patch E state ---- */
typedef void*       (*fn_pick_t)(void* R, uint64_t* id, void* taken);
static int         g_res_ok        = 0;   /* E targets resolved and verified                              */
static const char* g_res_reason    = "not resolved";
static int         g_patch_res     = 0;   /* 1 when the 16-byte jmp is installed                          */
static uintptr_t   g_res_pick = 0, g_res_wrap = 0, g_res_join = 0, g_res_resumed = 0, g_res_index = 0;
static uintptr_t   g_res_strlen = 0, g_res_cstr = 0, g_res_find = 0, g_res_setf = 0;
static uint32_t    g_res_off = 0;         /* EMPIRE_MP -> R                                                */
static unsigned char* g_tramp_res  = NULL;
static unsigned char  g_save_res[16];
static volatile LONG  g_res_calls = 0, g_res_shared = 0, g_res_vanilla = 0, g_res_errors = 0;
static char           g_res_last[256] = "none";

/* ---- Patch F state ---- */
typedef void          (*fn_issue_t)(void* env, unsigned char flag);
typedef void          (*fn_cancel_t)(void* env);
typedef void          (*fn_exec_t)(void);
typedef unsigned char (*fn_isturn_t)(void* etm, void* faction);
typedef unsigned char (*fn_find_t)(void* array, void** faction);
static int         g_et_ok         = 0;   /* F targets resolved and verified                              */
static const char* g_et_reason     = "not resolved";
static int         g_patch_et      = 0;   /* 1 when the hooks and the retargets are installed             */
static uintptr_t   g_et_exec = 0, g_et_issuer = 0, g_et_cancel = 0, g_et_find = 0, g_et_isturn = 0, g_et_can = 0, g_et_ccq = 0;
static uintptr_t   g_et_site[ET_SITES];
static unsigned char  g_et_save_site[ET_SITES][5];
static unsigned char  g_et_save_issuer[16], g_et_save_cancel[16];
static unsigned char* g_et_page    = NULL;  /* near page (rel32 reach): the thunk and both trampolines         */
#define ET_PAGE_ISSUER 0x40
#define ET_PAGE_CANCEL 0x80
static volatile LONG  g_et_shared  = 0;   /* Lua: the local faction is shared                             */
static volatile LONG  g_et_bypass  = 0;   /* Lua: consensus reached, the hooks pass everything            */
static void* volatile g_et_view    = NULL; /* the local faction while this machine has pressed, else NULL;
                                              read by the thunk, so it is always a plain pointer read      */
static unsigned char* volatile g_et_env = NULL;  /* env of the last press                                 */
static volatile LONG  g_et_calls = 0, g_et_presses = 0, g_et_passed = 0, g_et_cancels = 0, g_et_errors = 0;

/* ---- Patch G state ---- */
typedef uint32_t      (*fn_first_t)(void* local, uint32_t alliance, uint32_t army);
typedef void          (*fn_gift_sel_t)(void* env, uint32_t from, uint32_t to);
typedef void          (*fn_gift_units_t)(void* env, uint32_t from, uint32_t to, void* units);
typedef unsigned char (*fn_gift_send_t)(void* mp, uint32_t from, uint32_t to, void* entry, unsigned char keep);
typedef void          (*fn_gift_list_t)(void* players, uint32_t alliance, void* out);
typedef void*         (*fn_vget_t)(void* self);
typedef uint32_t      (*fn_vslot_t)(void* self);
enum { GF_S_ORIG_ARMY, GF_S_ORIG_FIRST, GF_S_OWNED_ARMY, GF_S_OWNED_FIRST, GF_S_GIFT, GF_S_RECALL, GF_S_SEND, GF_S_LIST, GF_SITES };
static const unsigned char GF_PAGE_OFF[GF_SITES] = { 0x00, 0x20, 0x00, 0x20, 0x30, 0x40, 0x50, 0x60 };
static int         g_gf_ok         = 0;   /* G targets resolved and verified                              */
static const char* g_gf_reason     = "not resolved";
static int         g_patch_gf      = 0;   /* 1 when the retargets are installed                           */
static uintptr_t   g_gf_env = 0, g_gf_army = 0, g_gf_first = 0, g_gf_sel = 0, g_gf_units = 0, g_gf_send = 0, g_gf_list = 0;
static uintptr_t   g_gf_site[GF_SITES], g_gf_want[GF_SITES];
static unsigned char  g_gf_save_site[GF_SITES][5];
static unsigned char* g_gf_page    = NULL;  /* near page: the context capture, then one stub per C function   */
static unsigned char* volatile g_gf_ctx = NULL;  /* the unit context the UI is asking about                   */
static volatile LONG  g_gf_keep = 0;      /* 1 during a recall: the send keeps the holder's copy              */
static volatile LONG  g_gf_answers = 0, g_gf_errors = 0;

/* ---- logging -------------------------------------------------------------------------------------- */
static void logline(const char* fmt, ...) {
    char buf[1024];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = _snprintf_s(buf, sizeof buf, _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d [archon_native] ",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf + n, sizeof buf - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    FILE* f = NULL;
    if (fopen_s(&f, "archon_native_log.txt", "a") == 0 && f) { fprintf(f, "%s\n", buf); fclose(f); }
}

static void hex6(char out[18], const unsigned char* b) {
    sprintf_s(out, 18, "%02X%02X%02X%02X%02X%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

/* ---- PE section walking (identity + scanning, read-only) ----------------------------------------- */
typedef struct { const unsigned char* base; DWORD va, vsz, chars; char name[9]; } sec_t;

static const IMAGE_NT_HEADERS* nt_of(HMODULE exe) {
    const unsigned char* b = (const unsigned char*)exe;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)b;
    return (const IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
}

static void fill_sec(HMODULE exe, const IMAGE_SECTION_HEADER* s, sec_t* out) {
    out->base  = (const unsigned char*)exe + s->VirtualAddress;
    out->va    = s->VirtualAddress;
    out->vsz   = s->Misc.VirtualSize;
    out->chars = s->Characteristics;
    memset(out->name, 0, sizeof out->name); memcpy(out->name, s->Name, 8);
}

static int find_section_for_scan(HMODULE exe, const char* name, sec_t* out) {
    const IMAGE_NT_HEADERS* nt = nt_of(exe);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        char nm[9] = {0}; memcpy(nm, s->Name, 8);
        if (strcmp(nm, name) == 0) { fill_sec(exe, s, out); return 1; }
    }
    return 0;
}

/* refuse the writable+executable anti-tamper region */
static int section_is_nonwritable_code(const sec_t* s) {
    return (s->chars & IMAGE_SCN_MEM_EXECUTE) && !(s->chars & IMAGE_SCN_MEM_WRITE);
}
static int section_is_readonly_data(const sec_t* s) {
    return (s->chars & IMAGE_SCN_MEM_READ) && !(s->chars & IMAGE_SCN_MEM_WRITE);
}
/* [va, va+len) lies inside section s */
static int in_sec(const sec_t* s, uintptr_t va, size_t len) {
    uintptr_t lo = g_imgbase + s->va, hi = lo + s->vsz;
    return va >= lo && va + len <= hi && va + len >= va;
}

/* parse "48 89 ?? .." into bytes[]/mask[], return count */
static int parse_sig(const char* sig, unsigned char* bytes, unsigned char* mask, int cap) {
    int n = 0; const char* p = sig;
    while (*p && n < cap) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?' ) { bytes[n] = 0; mask[n] = 0; ++n; p += (p[1]=='?')?2:1; }
        else { bytes[n] = (unsigned char)strtoul(p, NULL, 16); mask[n] = 0xFF; ++n; p += 2; }
    }
    return n;
}

/* scan a section for a masked signature; returns count, first hit VA in *hit */
static int scan_sig(const sec_t* s, const unsigned char* b, const unsigned char* m, int n, uintptr_t* hit) {
    int count = 0;
    if ((DWORD)n > s->vsz) return 0;
    for (DWORD i = 0; i + n <= s->vsz; ++i) {
        int ok = 1;
        for (int j = 0; j < n; ++j) if (m[j] && s->base[i + j] != b[j]) { ok = 0; break; }
        if (ok) { if (count == 0 && hit) *hit = g_imgbase + s->va + i; ++count; }
    }
    return count;
}

/* scan EVERY execute-but-not-writable section; total count, first hit + its section */
static int scan_sig_all_code(HMODULE exe, const unsigned char* b, const unsigned char* m, int n,
                             uintptr_t* hit, sec_t* hitsec) {
    const IMAGE_NT_HEADERS* nt = nt_of(exe);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    int total = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        sec_t sec; fill_sec(exe, s, &sec);
        if (!section_is_nonwritable_code(&sec)) continue;
        uintptr_t h = 0; int c = scan_sig(&sec, b, m, n, &h);
        if (c && total == 0) { if (hit) *hit = h; if (hitsec) *hitsec = sec; }
        total += c;
    }
    return total;
}

/* find an ASCII C-string in a read-only data section; returns VA or 0 */
static uintptr_t find_string(const sec_t* s, const char* str) {
    size_t len = strlen(str);
    if (len + 1 > s->vsz) return 0;
    for (DWORD i = 0; i + len + 1 <= s->vsz; ++i)
        if (s->base[i] == (unsigned char)str[0] && memcmp(s->base + i, str, len) == 0 && s->base[i + len] == 0)
            return g_imgbase + s->va + i;
    return 0;
}

/* count `lea rdx,[rip+disp32]` (48 8D 15 ..) in a code section that resolve to target; first hit in *hit */
static int count_lea_to(const sec_t* s, uintptr_t target, uintptr_t* hit) {
    int count = 0;
    for (DWORD i = 0; i + 7 <= s->vsz; ++i) {
        if (s->base[i] == 0x48 && s->base[i+1] == 0x8D && s->base[i+2] == 0x15) {
            int32_t disp; memcpy(&disp, s->base + i + 3, 4);
            uintptr_t insn = g_imgbase + s->va + i;
            if (insn + 7 + disp == target) { if (count == 0 && hit) *hit = insn; ++count; }
        }
    }
    return count;
}

/* rel32 branch target: insn_va + insn_len + rel32 at insn_va+rel_off */
static uintptr_t rel32_target(uintptr_t insn_va, int rel_off, int insn_len) {
    int32_t rel; memcpy(&rel, (const void*)(insn_va + rel_off), 4);
    return insn_va + insn_len + (intptr_t)rel;
}

/* find a UTF-16LE NUL-terminated string in a read-only data section; returns VA or 0 */
static uintptr_t find_wstring(const sec_t* s, const wchar_t* w) {
    size_t n = wcslen(w) * 2;
    if (n + 2 > s->vsz) return 0;
    for (DWORD i = 0; i + n + 2 <= s->vsz; i += 2)
        if (s->base[i] == (unsigned char)w[0] && memcmp(s->base + i, w, n) == 0 && s->base[i+n] == 0 && s->base[i+n+1] == 0)
            return g_imgbase + s->va + i;
    return 0;
}

/* count `lea r64,[rip+disp32]` (REX.W 48/4C, 8D, modrm mod=00 rm=101) in a code section resolving to target */
static int count_riplea_to(const sec_t* s, uintptr_t target, uintptr_t* hit) {
    int count = 0;
    for (DWORD i = 0; i + 7 <= s->vsz; ++i) {
        unsigned char r = s->base[i];
        if ((r == 0x48 || r == 0x4C) && s->base[i+1] == 0x8D && (s->base[i+2] & 0xC7) == 0x05) {
            int32_t disp; memcpy(&disp, s->base + i + 3, 4);
            uintptr_t insn = g_imgbase + s->va + i;
            if (insn + 7 + disp == target) { if (count == 0 && hit) *hit = insn; ++count; }
        }
    }
    return count;
}

/* masked compare at va (caller guarantees [va, va+n) is mapped) */
static int sig_at(uintptr_t va, const unsigned char* b, const unsigned char* m, int n) {
    const unsigned char* p = (const unsigned char*)va;
    for (int j = 0; j < n; ++j) if (m[j] && p[j] != b[j]) return 0;
    return 1;
}
/* parse + compare a text signature at va, bounds-checked against section s */
static int sigtext_at(const sec_t* s, uintptr_t va, const char* sig) {
    unsigned char b[256], m[256]; int n = parse_sig(sig, b, m, 256);
    return in_sec(s, va, (size_t)n) && sig_at(va, b, m, n);
}
/* count matches of a text signature in [lo, hi) (inside section s); first hit in *hit */
static int sigtext_count(const sec_t* s, uintptr_t lo, uintptr_t hi, const char* sig, uintptr_t* hit) {
    unsigned char b[256], m[256]; int n = parse_sig(sig, b, m, 256), c = 0;
    for (uintptr_t p = lo; p + n <= hi; ++p)
        if (in_sec(s, p, (size_t)n) && sig_at(p, b, m, n)) { if (c == 0 && hit) *hit = p; ++c; }
    return c;
}
static uint32_t rd32(uintptr_t va) { uint32_t v; memcpy(&v, (const void*)va, 4); return v; }

/* The section names come from the game's packer and may change with a game patch: by name, else by role. */
static int find_code_section(HMODULE exe, sec_t* out) {
    if (find_section_for_scan(exe, ".udata", out) && section_is_nonwritable_code(out)) return 1;
    const IMAGE_NT_HEADERS* nt = nt_of(exe);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    int found = 0; sec_t c;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        fill_sec(exe, s, &c);
        if (section_is_nonwritable_code(&c) && (!found || c.vsz > out->vsz)) { *out = c; found = 1; }
    }
    return found;
}

static int find_data_section(HMODULE exe, const char* key, sec_t* out) {
    if (find_section_for_scan(exe, ".xcode", out) && section_is_readonly_data(out) && find_string(out, key)) return 1;
    const IMAGE_NT_HEADERS* nt = nt_of(exe);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        fill_sec(exe, s, out);
        if (!section_is_readonly_data(out)) continue;
        __try { if (find_string(out, key)) return 1; }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return 0;
}

/* =================================================================================================== */
/* Patch C locator. Returns NULL on success, else a static reason string. Every read is bounds-checked
 * against .udata first.                                                                               */
static const char* resolve_producer(HMODULE exe, const sec_t* code) {
    unsigned char sb[96], sm[96];
    int n = parse_sig(PRODUCER_SIG, sb, sm, 96);
    uintptr_t hit = 0; sec_t hs;
    int c = scan_sig_all_code(exe, sb, sm, n, &hit, &hs);
    if (c != 1) { logline("producer signature count=%d (want 1)", c); return "producer signature not unique across non-writable code"; }
    if (hs.va != code->va || !in_sec(code, hit, (size_t)n)) return "producer signature hit is not in .udata";

    /* 1. the jne <dup block> */
    uintptr_t jne = hit + PSIG_JNE_OFF;
    const unsigned char* pj = (const unsigned char*)jne;
    if (pj[0] != 0x0F || pj[1] != 0x85) return "producer: no jne at signature offset";
    uintptr_t dup = rel32_target(jne, 2, 6);
    if (!in_sec(code, dup, 32)) return "producer: jne target outside .udata";

    /* 2. the current target must be the duplicate block: bts rax,0x1c within 16 bytes */
    int bts_at = -1;
    for (int i = 0; i <= 16; ++i) if (memcmp((const void*)(dup + i), BTS_RAX_1C, 5) == 0) { bts_at = i; break; }
    if (bts_at < 0) return "producer: jne target has no `bts rax,0x1c` within 16 bytes (already patched or wrong site)";

    /* 3. continue path, derived from the not-found branch's `jmp <continue>` */
    uintptr_t jmp = hit + PSIG_JMP_OFF;
    if (*(const unsigned char*)jmp != 0xE9) return "producer: no jmp at signature end";
    uintptr_t cont = rel32_target(jmp, 1, 5);
    if (!in_sec(code, cont, 7)) return "producer: continue target outside .udata";
    const unsigned char* pc = (const unsigned char*)cont;
    if (pc[0] != 0xFF || pc[1] != 0xC3 || pc[2] != 0xE9) return "producer: continue target is not `inc ebx; jmp`";

    /* 4. the duplicate block must fall through into the same continue path (… call rel32 ; <cont>) */
    if (cont <= dup || cont - dup > 0x40 || *(const unsigned char*)(cont - 5) != 0xE8)
        return "producer: duplicate block does not fall through into the continue path";

    /* 5. continue's jmp goes back to the loop head */
    uintptr_t head = rel32_target(cont + 2, 1, 5);
    if (!in_sec(code, head, sizeof LOOPHEAD_EXPECT)) return "producer: loop head outside .udata";
    if (memcmp((const void*)head, LOOPHEAD_EXPECT, sizeof LOOPHEAD_EXPECT) != 0) return "producer: loop head bytes mismatch";
    if (!(head < jne && jne - head < 0x200)) return "producer: loop head not shortly before the jne";

    /* 6. the inactive-slot `jne` between head and our jne must already target the same continue path */
    int inactive_ok = 0;
    for (uintptr_t p = head; p + 10 <= jne; ++p)
        if (memcmp((const void*)p, INACTIVE_EXPECT, 6) == 0 && rel32_target(p + 4, 2, 6) == cont) { inactive_ok = 1; break; }
    if (!inactive_ok) return "producer: inactive-slot jne to the continue path not found";

    /* 7. compute new bytes */
    intptr_t newrel = (intptr_t)cont - (intptr_t)(jne + 6);
    if (newrel < INT32_MIN || newrel > INT32_MAX) return "producer: rel32 out of range";
    int32_t r32 = (int32_t)newrel;
    memcpy(g_prod_orig, pj, 6);
    g_prod_new[0] = 0x0F; g_prod_new[1] = 0x85; memcpy(g_prod_new + 2, &r32, 4);

    g_producer = jne; g_prod_dup = dup; g_prod_cont = cont; g_prod_head = head;
    return NULL;
}

/* Patch D locator. Every address comes from code, anchored on
 * three strings; every offset the hook uses is read out of the engine's own instructions. NULL on success.  */
static const char* resolve_leave(HMODULE exe, const sec_t* code, const sec_t* xdata) {
    unsigned char b[128], m[128]; int n; uintptr_t hit = 0; int c;

    /* 1. player_left: UTF-16 log string -> unique lea -> the call sequence that sends the switch command */
    uintptr_t pl_str = find_wstring(xdata, PL_WSTR);
    if (!pl_str) return "leave: player_left string not found";
    uintptr_t pl_lea = 0;
    if ((c = count_riplea_to(code, pl_str, &pl_lea)) != 1) { logline("leave: player_left lea count=%d", c); return "leave: lea->player_left string not unique"; }
    uintptr_t call = 0;
    if ((c = sigtext_count(code, pl_lea, pl_lea + 0x200, PL_CALL_SIG, &call)) != 1) { logline("leave: send call sig in window count=%d", c); return "leave: send call signature not unique in player_left"; }
    n = parse_sig(PL_CALL_SIG, b, m, 128);
    if ((c = scan_sig_all_code(exe, b, m, n, NULL, NULL)) != 1) { logline("leave: send call sig global count=%d", c); return "leave: send call signature not unique across non-writable code"; }
    g_leave_pl     = pl_lea;
    g_off_setup    = rd32(call + PLC_SETUP_DISP) + 8;            /* rdi = this+8 (verified by lea rcx,[rdi-8]) */
    g_off_setup_sub= *(const unsigned char*)(call + PLC_SETUP_SUB);
    g_leave_getkey = rel32_target(call + PLC_GETKEY_E8, 1, 5);
    g_leave_send   = rel32_target(call + PLC_SEND_E8, 1, 5);

    /* 2. player_left: player list offset + lookup(list,&id); the "controls a faction" flag predicate */
    uintptr_t ls = 0;
    if ((c = sigtext_count(code, pl_lea, call, PL_LIST_SIG, &ls)) != 1) return "leave: player list load not found in player_left";
    g_off_list     = rd32(ls + 3) + 8;
    g_leave_lookup = rel32_target(ls + 13, 1, 5);
    if (sigtext_count(code, ls, call, PL_FLAGS_SIG, NULL) != 1) return "leave: leaving-record flag predicate (0x138 & 0x11/0x25) not found";

    /* 3. lookup -> id map offset + rec_at; rec_at exact bytes fix count 0x2800 / stride 0x140 / flags 0x138 */
    if (!sigtext_at(code, g_leave_lookup, LOOKUP_SIG)) return "leave: lookup(list,&id) bytes mismatch";
    g_off_map     = rd32(g_leave_lookup + 14);
    g_leave_recat = rel32_target(g_leave_lookup + 18, 1, 5);
    if (!sigtext_at(code, g_leave_recat, RECAT_SIG)) return "leave: rec_at(list,idx) bytes mismatch";
    if (g_off_map < NP_COUNT_OFF || g_off_map > NP_COUNT_OFF + 0x100) return "leave: id map offset implausible";

    /* 4. getkey(rec, setup): [rec+0x90] indexes the per-player setup list, key = [entry+8] */
    if (!sigtext_at(code, g_leave_getkey, GETKEY_SIG)) return "leave: getkey(rec,setup) bytes mismatch";

    /* 5. handle_player_left: local player id chain, and the same list offset */
    uintptr_t hpl_str = find_wstring(xdata, HPL_WSTR), hpl_lea = 0, loc = 0, hl = 0;
    if (!hpl_str) return "leave: handle_player_left string not found";
    if (count_riplea_to(code, hpl_str, &hpl_lea) != 1) return "leave: lea->handle_player_left string not unique";
    if (sigtext_count(code, hpl_lea, hpl_lea + 0x100, HPL_LOCAL_SIG, &loc) != 1) return "leave: local player id chain not found";
    g_off_loc_a = rd32(loc + 3); g_off_loc_b = *(const unsigned char*)(loc + 13); g_off_loc_c = rd32(loc + 16);
    if (sigtext_count(code, hpl_lea, hpl_lea + 0x100, HPL_LIST_SIG, &hl) != 1) return "leave: list load not found in handle_player_left";
    if (rd32(hl + 3) != g_off_list) return "leave: player list offset differs between player_left and handle_player_left";

    /* 6. the sender itself; its inner command builder reads the command id from one slot */
    if (!sigtext_at(code, g_leave_send, SEND_SIG)) return "leave: sender bytes mismatch (already hooked?)";
    if (memcmp((const void*)g_leave_send, SEND_PROLOGUE, 16) != 0) return "leave: sender prologue mismatch";
    uintptr_t inner = rel32_target(g_leave_send + SEND_INNER_E8, 1, 5);
    if (!sigtext_at(code, inner, INNER_SIG)) return "leave: command builder bytes mismatch";
    uintptr_t slot1 = inner + 16 + (int32_t)rd32(inner + 12), slot2 = inner + 43 + (int32_t)rd32(inner + 39);
    if (slot1 != slot2) return "leave: command builder reads two different id slots";
    g_leave_slot = slot1;

    /* 7. the slot must be the one registered for "CCQ_FACTION_SWITCH_HUMAN_TO_AI"; follow its executor down to
     *    the IDLE_HUMAN write (mov byte [rsi+0x1bc4],1)                                                     */
    uintptr_t ccq = find_string(xdata, CCQ_STR), ccq_lea = 0;
    if (!ccq) return "leave: CCQ_FACTION_SWITCH_HUMAN_TO_AI string not found";
    if (count_lea_to(code, ccq, &ccq_lea) != 1) return "leave: lea rdx->CCQ string not unique";
    if (!in_sec(code, ccq_lea - 10, 24)) return "leave: CCQ registration out of bounds";
    if (memcmp((const void*)(ccq_lea - 10), CCQ_PRE, 3) || memcmp((const void*)(ccq_lea - 3), CCQ_MID, 3) ||
        memcmp((const void*)(ccq_lea + 7), CCQ_POST, 3)) return "leave: CCQ registration shape mismatch";
    if (ccq_lea + 14 + (int32_t)rd32(ccq_lea + 10) != g_leave_slot) return "leave: sender does not send CCQ_FACTION_SWITCH_HUMAN_TO_AI";
    g_leave_handler = ccq_lea - 3 + (int32_t)rd32(ccq_lea - 7);
    if (sigtext_count(code, g_leave_handler, g_leave_handler + 0x6e, HANDLER_CALL_SIG, &hit) != 1) return "leave: executor call to world switch not found";
    g_leave_world = rel32_target(hit + 6, 1, 5);
    if (sigtext_count(code, g_leave_world, g_leave_world + 0x20, WORLD_CALL_SIG, &hit) != 1) return "leave: world switch call to set_idle not found";
    g_leave_setidle = rel32_target(hit + 3, 1, 5);
    if (sigtext_count(code, g_leave_setidle, g_leave_setidle + 0x100, SETIDLE_SIG, NULL) != 1) return "leave: set_idle does not write IDLE_HUMAN to +0x1bc4";

    /* 8. engine string accessors, reached from the payload vtable's serialiser */
    uintptr_t pvt = g_leave_send + SEND_PAYLOAD_LEA + 7 + (int32_t)rd32(g_leave_send + SEND_PAYLOAD_LEA + 3);
    if (!in_sec(xdata, pvt, 8)) return "leave: payload vtable not in .xcode";
    uintptr_t vt0; memcpy(&vt0, (const void*)pvt, 8);
    if (!in_sec(code, vt0, 32))   /* absolute pointer: rebase if the image is not at its preferred base */
        vt0 = vt0 - (uintptr_t)nt_of(exe)->OptionalHeader.ImageBase + g_imgbase;
    if (!sigtext_at(code, vt0, VT0_SIG)) return "leave: payload serialiser bytes mismatch";
    uintptr_t ser = rel32_target(vt0 + 14, 1, 5);
    if (!sigtext_at(code, ser, SER_SIG)) return "leave: string serialiser bytes mismatch";
    g_leave_strlen = rel32_target(ser + SER_LEN_E8, 1, 5);
    if (!sigtext_at(code, ser + SER_CSTR_OFF, "48 8B CF E8")) return "leave: c_str call not found in serialiser";
    g_leave_cstr = rel32_target(ser + SER_CSTR_OFF + 3, 1, 5);
    if (!sigtext_at(code, g_leave_strlen, STRLEN_SIG) || !sigtext_at(code, g_leave_cstr, CSTR_SIG)) return "leave: string accessor bytes mismatch";

    /* 9. no target may lie in writable memory */
    uintptr_t all[] = { g_leave_send, g_leave_getkey, g_leave_lookup, g_leave_recat, g_leave_strlen, g_leave_cstr };
    for (int i = 0; i < (int)(sizeof all / sizeof all[0]); ++i) if (!in_sec(code, all[i], 16)) return "leave: a resolved function is outside .udata";
    return NULL;
}

/* Patch E locator. Anchored on two UTF-16 log strings; every
 * offset the hook relies on is fixed by the picker's exact bytes. NULL on success.                          */
static const char* resolve_resume(HMODULE exe, const sec_t* code, const sec_t* xdata) {
    unsigned char b[256], m[256]; int n, c;

    /* 1. host join handler (ADD_FILLED_PLAYER_SLOT): the resumed seating sequence */
    uintptr_t js = find_wstring(xdata, JOIN_WSTR), jl = 0, j = 0;
    if (!js) return "resume: ADD_FILLED_PLAYER_SLOT string not found";
    if ((c = count_riplea_to(code, js, &jl)) != 1) { logline("resume: join lea count=%d", c); return "resume: lea->ADD_FILLED_PLAYER_SLOT string not unique"; }
    if ((c = sigtext_count(code, jl, jl + 0x200, JOIN_SIG, &j)) != 1) { logline("resume: join sig in window count=%d", c); return "resume: resumed seating sequence not unique in the join handler"; }
    n = parse_sig(JOIN_SIG, b, m, 256);
    if ((c = scan_sig_all_code(exe, b, m, n, NULL, NULL)) != 1) { logline("resume: join sig global count=%d", c); return "resume: resumed seating sequence not unique across non-writable code"; }
    g_res_join    = j;
    g_res_off     = rd32(j + 3);
    g_res_resumed = rel32_target(j + 10, 1, 5);
    g_res_wrap    = rel32_target(j + 26, 1, 5);
    g_res_index   = rel32_target(j + 37, 1, 5);

    /* 2. the wrapper (builds the taken-keys list from the seated slots) calls the picker on the same R */
    uintptr_t wc = 0;
    if (sigtext_count(code, g_res_wrap, g_res_wrap + 0x110, WRAP_CALL_SIG, &wc) != 1) return "resume: picker call not found in the seating wrapper";
    if (rd32(wc + 3) != g_res_off) return "resume: wrapper uses another resume-info offset";
    g_res_pick = rel32_target(wc + 14, 1, 5);

    /* 3. the picker: exact bytes (fixes every offset the hook reads), unique, relocatable prologue */
    if (!sigtext_at(code, g_res_pick, PICK_SIG)) return "resume: picker bytes mismatch (already hooked?)";
    n = parse_sig(PICK_SIG, b, m, 256);
    if ((c = scan_sig_all_code(exe, b, m, n, NULL, NULL)) != 1) { logline("resume: picker sig global count=%d", c); return "resume: picker signature not unique"; }
    if (memcmp((const void*)g_res_pick, PICK_PROLOGUE, 16) != 0) return "resume: picker prologue mismatch";
    g_res_find = rel32_target(g_res_pick + PICK_FIND1_E8, 1, 5);
    if (rel32_target(g_res_pick + PICK_FIND2_E8, 1, 5) != g_res_find) return "resume: picker uses two different find helpers";

    /* 4. is_resumed(R) gives the strlen accessor; index_of(R,key) confirms the key array layout */
    if (!sigtext_at(code, g_res_resumed, RESUMED_SIG)) return "resume: is_resumed bytes mismatch";
    g_res_strlen = rel32_target(g_res_resumed + 13, 1, 5);
    if (!sigtext_at(code, g_res_strlen, STRLEN_SIG)) return "resume: strlen accessor bytes mismatch";
    if (!sigtext_at(code, g_res_index, INDEX_SIG)) return "resume: index_of bytes mismatch";

    /* 5. EMPIRE_MP::set_faction (second anchor): c_str accessor; same R offset, is_resumed and index_of */
    uintptr_t ss = find_wstring(xdata, SETF_WSTR), sl = 0, sr = 0;
    if (!ss) return "resume: set_faction log string not found";
    if ((c = count_riplea_to(code, ss, &sl)) != 1) { logline("resume: set_faction lea count=%d", c); return "resume: lea->set_faction log string not unique"; }
    if (!sigtext_at(code, sl - 0xd, "49 8B CF E8")) return "resume: c_str call not found before the set_faction log";
    g_res_cstr = rel32_target(sl - 0xd + 3, 1, 5);
    if (!sigtext_at(code, g_res_cstr, CSTR_SIG)) return "resume: c_str accessor bytes mismatch";
    if (sigtext_count(code, sl - 0x80, sl, SETF_RES_SIG, &sr) != 1) return "resume: set_faction resumed branch not found";
    if (rd32(sr + 3) != g_res_off || rel32_target(sr + 10, 1, 5) != g_res_resumed || rel32_target(sr + 25, 1, 5) != g_res_index)
        return "resume: set_faction and the join handler disagree (R offset / is_resumed / index_of)";
    g_res_setf = sl;

    /* 6. every function used lies in non-writable code */
    uintptr_t all[] = { g_res_pick, g_res_wrap, g_res_resumed, g_res_index, g_res_strlen, g_res_cstr, g_res_find };
    for (int i = 0; i < (int)(sizeof all / sizeof all[0]); ++i) if (!in_sec(code, all[i], 16)) return "resume: a resolved function is outside .udata";
    if (g_res_off < 0x1000 || g_res_off > 0x200000) return "resume: resume-info offset implausible";
    return NULL;
}

/* Patch F locator. The exact-byte signatures of the issuer and
 * the cancel issuer fix every offset the hooks read. All five readers must call the same find. NULL on success.  */
static const char* resolve_endturn(HMODULE exe, const sec_t* code, const sec_t* xdata) {
    unsigned char b[256], m[256]; int n, c; uintptr_t hit = 0; sec_t hs;

    /* 1. executor (unique whole function) -> can_end_turn + issuer -> is_factions_turn */
    n = parse_sig(ET_EXEC_SIG, b, m, 256);
    if ((c = scan_sig_all_code(exe, b, m, n, &hit, &hs)) != 1) { logline("endturn: executor sig count=%d", c); return "endturn: EndTurn executor signature not unique"; }
    if (hs.va != code->va) return "endturn: EndTurn executor not in .udata";
    g_et_exec   = hit;
    g_et_can    = rel32_target(hit + ET_EXEC_CAN_E8, 1, 5);
    g_et_issuer = rel32_target(hit + ET_EXEC_ISS_E8, 1, 5);
    if (!sigtext_at(code, g_et_issuer, ET_ISSUER_SIG)) return "endturn: issuer bytes mismatch (already hooked?)";
    if (memcmp((const void*)g_et_issuer, ET_ISSUER_PROLOGUE, 18) != 0) return "endturn: issuer prologue mismatch";
    g_et_isturn = rel32_target(g_et_issuer + ET_ISS_TURN_E8, 1, 5);

    /* 2. CCQ_END_TURN registration: its executor uses the same can_end_turn */
    uintptr_t ccq = find_string(xdata, ET_CCQ_STR), ccq_lea = 0;
    if (!ccq) return "endturn: CCQ_END_TURN string not found";
    if (count_lea_to(code, ccq, &ccq_lea) != 1) return "endturn: lea rdx->CCQ_END_TURN not unique";
    if (!in_sec(code, ccq_lea - 10, 24)) return "endturn: CCQ registration out of bounds";
    if (memcmp((const void*)(ccq_lea - 10), CCQ_PRE, 3) || memcmp((const void*)(ccq_lea - 3), CCQ_MID, 3))
        return "endturn: CCQ registration shape mismatch";
    g_et_ccq = ccq_lea - 3 + (int32_t)rd32(ccq_lea - 7);
    if (sigtext_count(code, g_et_ccq, g_et_ccq + 0xb0, ET_CCQ_CALL_SIG, &hit) != 1) return "endturn: can_end_turn call not found in the CCQ executor";
    if (rel32_target(hit + ET_CCQ_CAN_E8, 1, 5) != g_et_can) return "endturn: executors disagree on can_end_turn";

    /* 3. getter site, anchored on its registration: lea rax,<getter> ... lea rdx,"IsFactionReadyToEndTurn" */
    uintptr_t rs = find_string(xdata, ET_READY_STR), rl = 0;
    if (!rs) return "endturn: IsFactionReadyToEndTurn string not found";
    if (count_lea_to(code, rs, &rl) != 1) return "endturn: lea rdx->IsFactionReadyToEndTurn not unique";
    if (!sigtext_at(code, rl - 10, "48 8D 05 ?? ?? ?? ?? 45 33 F6")) return "endturn: getter registration shape mismatch";
    uintptr_t getter = rl - 3 + (int32_t)rd32(rl - 7);
    if (sigtext_count(code, getter, getter + 0x60, ET_SITE_RCX_SIG, &hit) != 1) return "endturn: reader not found in the getter";
    g_et_site[0] = hit + ET_SITE_E8;

    /* 4. EndTurnButton: lea rdx,<name> ... lea rax,<vtable> -> state updater + click handler -> cancel issuer */
    uintptr_t bs = find_string(xdata, ET_BUTTON_STR), bl = 0;
    if (!bs) return "endturn: EndTurnButton string not found";
    c = 0;
    for (DWORD i = 0; i + 7 <= code->vsz; ++i) {                 /* the name has three leas, one is the factory */
        if (code->base[i] != 0x48 || code->base[i+1] != 0x8D || code->base[i+2] != 0x15) continue;
        uintptr_t insn = g_imgbase + code->va + i;
        if (insn + 7 + (int32_t)rd32(insn + 3) == bs && sigtext_at(code, insn + 7, ET_FACTORY_SIG)) { bl = insn; ++c; }
    }
    if (c != 1) { logline("endturn: EndTurnButton factory count=%d", c); return "endturn: EndTurnButton factory not unique"; }
    uintptr_t vt = bl + 15 + 7 + (int32_t)rd32(bl + 15 + 3);
    if (!in_sec(xdata, vt, (ET_VT_CLICK + 1) * 8)) return "endturn: EndTurnButton vtable not in .xcode";
    uintptr_t upd, clk, delta = g_imgbase - (uintptr_t)nt_of(exe)->OptionalHeader.ImageBase;
    memcpy(&upd, (const void*)(vt + ET_VT_UPDATE * 8), 8); upd += delta;
    memcpy(&clk, (const void*)(vt + ET_VT_CLICK  * 8), 8); clk += delta;
    if (!in_sec(code, upd, 0x110) || !in_sec(code, clk, 0x110)) return "endturn: EndTurnButton vtable slots outside .udata";
    if (sigtext_count(code, clk, clk + 0x100, ET_SITE_RAX_SIG, &hit) != 1) return "endturn: reader not found in the click handler";
    g_et_site[1] = hit + ET_SITE_E8;
    if (!sigtext_at(code, g_et_site[1] + 5, ET_CLICK_CANCEL_SIG)) return "endturn: cancel call not found after the click reader";
    g_et_cancel = rel32_target(g_et_site[1] + 5 + ET_CLK_CALL_E8, 1, 5);
    if (sigtext_count(code, upd, upd + 0x110, ET_SITE_RAX_SIG, &hit) != 1) return "endturn: reader not found in the state updater";
    g_et_site[2] = hit + ET_SITE_E8;

    /* 5. the cancel issuer: exact bytes; its own reader gives find, and stays untouched (the hook runs first) */
    if (!sigtext_at(code, g_et_cancel, ET_CANCEL_SIG)) return "endturn: cancel issuer bytes mismatch (already hooked?)";
    if (memcmp((const void*)g_et_cancel, ET_CANCEL_PROLOGUE, 18) != 0) return "endturn: cancel issuer prologue mismatch";
    g_et_find = rel32_target(g_et_cancel + ET_CAN_FIND_E8, 1, 5);
    if (!sigtext_at(code, g_et_find, ET_FIND_SIG)) return "endturn: find bytes mismatch";

    /* 6. the two remaining readers: exactly two hits of one signature, in address order */
    n = parse_sig(ET_SITE_R11_SIG, b, m, 256);
    if ((c = scan_sig(code, b, m, n, &hit)) != 2) { logline("endturn: r11 reader count=%d", c); return "endturn: expected exactly two turn-gated ready readers"; }
    { int k = 3; uintptr_t lo = hit, hi = g_imgbase + code->va + code->vsz, h2 = 0;
      g_et_site[k++] = lo + ET_R11_E8;
      if (sigtext_count(code, lo + 1, hi, ET_SITE_R11_SIG, &h2) != 1) return "endturn: second turn-gated reader not found";
      g_et_site[k]   = h2 + ET_R11_E8; }

    /* 7. every site really is `call find` */
    for (int i = 0; i < ET_SITES; ++i) {
        if (!in_sec(code, g_et_site[i], 5) || *(const unsigned char*)g_et_site[i] != 0xE8) return "endturn: a reader site is not a call";
        if (rel32_target(g_et_site[i], 1, 5) != g_et_find) return "endturn: a reader site does not call find";
    }
    uintptr_t all[] = { g_et_exec, g_et_issuer, g_et_cancel, g_et_find, g_et_isturn };
    for (int i = 0; i < (int)(sizeof all / sizeof all[0]); ++i) if (!in_sec(code, all[i], 16)) return "endturn: a resolved function is outside .udata";
    return NULL;
}

/* Patch G locator: six exact signatures, cross-checked on the shared callees. NULL on success. */
static const char* resolve_gift(const sec_t* code) {
    struct { const char* sig; const char* name; uintptr_t hit; } s[] = {
        { GF_ORIG_SIG, "original owner", 0 }, { GF_OWNED_SIG, "owned by local", 0 }, { GF_GIFT_SIG, "GiftUnits", 0 },
        { GF_RECALL_SIG, "recall", 0 }, { GF_SEND_SIG, "gift send", 0 }, { GF_LIST_SIG, "player list", 0 } };
    unsigned char b[256], m[256]; int n, c;
    for (int i = 0; i < 6; ++i) {
        n = parse_sig(s[i].sig, b, m, 256);
        if ((c = scan_sig(code, b, m, n, &s[i].hit)) != 1) { logline("gift: %s count=%d", s[i].name, c); return "gift: a signature is not unique"; }
    }
    g_gf_env = rel32_target(s[0].hit, 3, 7);
    g_gf_site[GF_S_ORIG_ARMY]   = s[0].hit + GF_ORIG_ARMY_E8;
    g_gf_site[GF_S_ORIG_FIRST]  = s[0].hit + GF_ORIG_FIRST_E8;
    g_gf_site[GF_S_OWNED_ARMY]  = s[1].hit + GF_OWNED_ARMY_E8;
    g_gf_site[GF_S_OWNED_FIRST] = s[1].hit + GF_OWNED_FIRST_E8;
    g_gf_site[GF_S_GIFT]        = s[2].hit + GF_GIFT_E8;
    g_gf_site[GF_S_RECALL]      = s[3].hit + GF_RECALL_E8;
    g_gf_site[GF_S_SEND]        = s[4].hit + GF_SEND_E8;
    g_gf_site[GF_S_LIST]        = s[5].hit + GF_LIST_E8;
    for (int i = 0; i < GF_SITES; ++i) {
        if (!in_sec(code, g_gf_site[i], 5)) return "gift: a site is outside .udata";
        g_gf_want[i] = rel32_target(g_gf_site[i], 1, 5);
        if (!in_sec(code, g_gf_want[i], 16)) return "gift: a target is outside .udata";
    }
    g_gf_army  = g_gf_want[GF_S_ORIG_ARMY];
    g_gf_first = g_gf_want[GF_S_ORIG_FIRST];
    g_gf_sel   = g_gf_want[GF_S_GIFT];
    g_gf_units = g_gf_want[GF_S_RECALL];
    g_gf_send  = g_gf_want[GF_S_SEND];
    g_gf_list  = g_gf_want[GF_S_LIST];
    if (g_gf_want[GF_S_OWNED_ARMY] != g_gf_army || g_gf_want[GF_S_OWNED_FIRST] != g_gf_first) return "gift: the two owner functions disagree";
    if (rel32_target(g_gf_sel + GF_SEL_E8, 1, 5) != g_gf_units) return "gift: GiftUnits does not reach gift_units";
    if (g_gf_site[GF_S_SEND] - g_gf_units >= GF_UNITS_SPAN) return "gift: the send is not in gift_units";
    return NULL;
}

/* One-time init: bind API, pin module, verify host, resolve targets. Never throws.                    */
static void do_init(void) {
    HMODULE exe = GetModuleHandleW(NULL);

    /* bind Lua API by name */
    #define BIND(field, name) *(FARPROC*)&A.field = GetProcAddress(exe, name)
    BIND(createtable,"lua_createtable"); BIND(setfield,"lua_setfield"); BIND(pushboolean,"lua_pushboolean");
    BIND(pushinteger,"lua_pushinteger"); BIND(pushlstring,"lua_pushlstring"); BIND(pushcclosure,"lua_pushcclosure");
    BIND(toboolean,"lua_toboolean");
    #undef BIND
    if (!A.createtable || !A.setfield || !A.pushcclosure || !A.pushlstring || !A.pushboolean || !A.pushinteger || !A.toboolean) {
        g_reason = "could not bind Lua C API from Warhammer3.exe exports"; logline("%s", g_reason); return;
    }

    /* pin ourselves so we survive lua_close at every mode switch */
    { HMODULE self = NULL;
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                         (LPCWSTR)&do_init, &self); }

    /* ---- host identity: reported, never enforced ---- */
    const IMAGE_NT_HEADERS* nt = nt_of(exe);
    g_imgbase = (uintptr_t)exe;                 /* address what is actually mapped */
    DWORD ts  = nt->FileHeader.TimeDateStamp;
    DWORD soi = nt->OptionalHeader.SizeOfImage;
    g_exe_tested = ts == TESTED_TIMESTAMP && soi == TESTED_SIZEOFIMAGE;
    if (!g_exe_tested) logline("WARNING: untested game version (timestamp 0x%08x, SizeOfImage 0x%x), at the player's own risk; a patch whose signatures do not resolve stays off", ts, soi);

    /* ---- locate sections ---- */
    sec_t code, xdata;
    if (!find_code_section(exe, &code))             { g_reason = "no code section to scan"; logline("%s", g_reason); return; }
    if (!find_data_section(exe, DUP_KEY, &xdata))   { g_reason = "no read-only section holds the anchor string"; logline("%s", g_reason); return; }
    if (strcmp(code.name, ".udata") != 0 || strcmp(xdata.name, ".xcode") != 0) logline("note: sections by role: code=%s data=%s", code.name, xdata.name);

    /* ---- Patch A target: unique sig:canset in .udata ---- */
    { unsigned char sb[48], sm[48];
      int n = parse_sig(CANSET_SIG, sb, sm, 48);
      uintptr_t hit = 0; int c = scan_sig(&code, sb, sm, n, &hit);
      if (c != 1) { g_reason = "CanSetFaction signature not unique"; logline("%s (count=%d)", g_reason, c); return; }
      g_canset = hit; }

    /* ---- Patch B target: string-anchored unique lea, then bt site ---- */
    g_dupstr = find_string(&xdata, DUP_KEY);
    if (!g_dupstr) { g_reason = "duplicate-faction loc key string not found"; logline("%s", g_reason); return; }
    { uintptr_t leahit = 0; int c = count_lea_to(&code, g_dupstr, &leahit);
      if (c != 1) { g_reason = "lea->dup-string xref not unique"; logline("%s (count=%d)", g_reason, c); return; }
      g_lea = leahit;
      g_bt  = leahit - LEA_TO_BT_DELTA;
      const unsigned char* site = (const unsigned char*)g_bt;
      if (memcmp(site, BT_EXPECT, 5) != 0) { g_reason = "bytes at bt site are not `bt rax,0x1c`"; logline("%s", g_reason); return; } }

    /* ---- Patch C target: ready-mask producer ---- */
    { const char* why = resolve_producer(exe, &code);
      if (why) { g_reason = why; logline("%s", why); return; } }

    g_host_ok = 1; g_reason = "ok";
    { char o[18], nw[18]; hex6(o, g_prod_orig); hex6(nw, g_prod_new);
      logline("init ok: version=%s imgbase=0x%llx canset=0x%llx dupstr=0x%llx lea=0x%llx bt=0x%llx "
              "producer_jne=0x%llx dup=0x%llx cont=0x%llx head=0x%llx bytes %s->%s",
              ARCHON_VERSION, (unsigned long long)g_imgbase, (unsigned long long)g_canset,
              (unsigned long long)g_dupstr, (unsigned long long)g_lea, (unsigned long long)g_bt,
              (unsigned long long)g_producer, (unsigned long long)g_prod_dup,
              (unsigned long long)g_prod_cont, (unsigned long long)g_prod_head, o, nw); }
    if (g_canset != REF_CANSET_VA || g_lea != REF_LEA_VA || g_bt != REF_BT_VA || g_dupstr != REF_DUPSTRING_VA ||
        g_producer != REF_PRODUCER_JNE_VA || g_prod_cont != REF_PRODUCER_CONT_VA)
        logline("WARNING: resolved addresses differ from the reference VAs");

    /* ---- Patch D target (independent: A/B/C stay usable if D does not resolve) ---- */
    { const char* why = resolve_leave(exe, &code, &xdata);
      if (why) { g_leave_ok = 0; g_leave_reason = why; logline("patch D unresolved: %s", why); }
      else {
          g_leave_ok = 1; g_leave_reason = "ok";
          logline("patch D resolved: send=0x%llx getkey=0x%llx lookup=0x%llx rec_at=0x%llx strlen=0x%llx c_str=0x%llx "
                  "slot=0x%llx handler=0x%llx world=0x%llx set_idle=0x%llx offsets list=0x%x setup=0x%x+0x%x map=0x%x local=0x%x/0x%x/0x%x",
                  (unsigned long long)g_leave_send, (unsigned long long)g_leave_getkey, (unsigned long long)g_leave_lookup,
                  (unsigned long long)g_leave_recat, (unsigned long long)g_leave_strlen, (unsigned long long)g_leave_cstr,
                  (unsigned long long)g_leave_slot, (unsigned long long)g_leave_handler, (unsigned long long)g_leave_world,
                  (unsigned long long)g_leave_setidle, g_off_list, g_off_setup, g_off_setup_sub, g_off_map,
                  g_off_loc_a, g_off_loc_b, g_off_loc_c);
          if (g_leave_send != REF_LEAVE_SEND_VA || g_leave_handler != REF_LEAVE_HANDLER_VA || g_leave_setidle != REF_LEAVE_SETIDLE_VA)
              logline("WARNING: patch D addresses differ from the reference VAs");
      } }

    /* ---- Patch E target (independent of A/B/C/D) ---- */
    { const char* why = resolve_resume(exe, &code, &xdata);
      if (why) { g_res_ok = 0; g_res_reason = why; logline("patch E unresolved: %s", why); }
      else {
          g_res_ok = 1; g_res_reason = "ok";
          logline("patch E resolved: picker=0x%llx wrapper=0x%llx join=0x%llx R=EMPIRE_MP+0x%x is_resumed=0x%llx index_of=0x%llx "
                  "find=0x%llx strlen=0x%llx c_str=0x%llx set_faction_log_lea=0x%llx",
                  (unsigned long long)g_res_pick, (unsigned long long)g_res_wrap, (unsigned long long)g_res_join, g_res_off,
                  (unsigned long long)g_res_resumed, (unsigned long long)g_res_index, (unsigned long long)g_res_find,
                  (unsigned long long)g_res_strlen, (unsigned long long)g_res_cstr, (unsigned long long)g_res_setf);
          if (g_res_pick != REF_RES_PICK_VA || g_res_wrap != REF_RES_WRAP_VA || g_res_join != REF_RES_JOIN_VA || g_res_off != REF_RES_OFF)
              logline("WARNING: patch E addresses differ from the reference VAs");
      } }

    /* ---- Patch F target (independent of A/B/C/D/E) ---- */
    { const char* why = resolve_endturn(exe, &code, &xdata);
      if (why) { g_et_ok = 0; g_et_reason = why; logline("patch F unresolved: %s", why); }
      else {
          g_et_ok = 1; g_et_reason = "ok";
          logline("patch F resolved: exec=0x%llx issuer=0x%llx cancel=0x%llx find=0x%llx is_turn=0x%llx can_end=0x%llx ccq=0x%llx "
                  "sites=0x%llx 0x%llx 0x%llx 0x%llx 0x%llx",
                  (unsigned long long)g_et_exec, (unsigned long long)g_et_issuer, (unsigned long long)g_et_cancel,
                  (unsigned long long)g_et_find, (unsigned long long)g_et_isturn, (unsigned long long)g_et_can,
                  (unsigned long long)g_et_ccq, (unsigned long long)g_et_site[0], (unsigned long long)g_et_site[1],
                  (unsigned long long)g_et_site[2], (unsigned long long)g_et_site[3], (unsigned long long)g_et_site[4]);
          int same = g_et_exec == REF_ET_EXEC_VA && g_et_issuer == REF_ET_ISSUER_VA && g_et_cancel == REF_ET_CANCEL_VA &&
                     g_et_find == REF_ET_FIND_VA && g_et_ccq == REF_ET_CCQ_VA;
          for (int i = 0; i < ET_SITES; ++i) {
              int found = 0;
              for (int j = 0; j < ET_SITES; ++j) if (g_et_site[i] == REF_ET_SITES[j]) found = 1;
              same = same && found;
          }
          if (!same) logline("WARNING: patch F addresses differ from the reference VAs");
      } }

    /* ---- Patch G target (independent of A to F) ---- */
    { const char* why = resolve_gift(&code);
      if (why) { g_gf_ok = 0; g_gf_reason = why; logline("patch G unresolved: %s", why); }
      else {
          g_gf_ok = 1; g_gf_reason = "ok";
          logline("patch G resolved: env=0x%llx first=0x%llx sel=0x%llx units=0x%llx send=0x%llx list=0x%llx",
                  (unsigned long long)g_gf_env, (unsigned long long)g_gf_first, (unsigned long long)g_gf_sel,
                  (unsigned long long)g_gf_units, (unsigned long long)g_gf_send, (unsigned long long)g_gf_list);
      } }
}

/* ---- apply/restore a single patch under VirtualProtect, saving originals ------------------------- */
static int write_bytes(uintptr_t va, const unsigned char* src, SIZE_T len, unsigned char* save_out) {
    DWORD old;
    if (!VirtualProtect((LPVOID)va, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    if (save_out) memcpy(save_out, (void*)va, len);
    memcpy((void*)va, src, len);
    DWORD tmp; VirtualProtect((LPVOID)va, len, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (LPVOID)va, len);
    return 1;
}

/* =================================================================================================== */
/* Patch D: the hook. Runs on the thread that would have sent CCQ_FACTION_SWITCH_HUMAN_TO_AI.            */

/* the engine's 16-byte string: length + pointer, read through the engine's own accessors */
static int key_text(void* s, const char** p, uint32_t* len) {
    if (!s) return 0;
    *len = ((fn_strlen_t)g_leave_strlen)(s);
    *p   = ((fn_cstr_t)g_leave_cstr)(s);
    if (*len > 255 || (!*p && *len)) return 0;
    return 1;
}
static int key_eq(void* a, void* b) {
    const char *pa, *pb; uint32_t la, lb;
    if (a == b) return 1;
    if (!key_text(a, &pa, &la) || !key_text(b, &pb, &lb)) return 0;
    return la == lb && (la == 0 || memcmp(pa, pb, la) == 0);
}
/* same predicate player_left applies to the leaving record before it asks for the switch */
static int controls_faction(const unsigned char* rec) {
    uint32_t f = *(const uint32_t*)(rec + NP_FLAGS_OFF);
    return (f & 0x11) == 1 && (f & 0x25) != 0x25;
}

/* returns 1 = keep the faction human (do not send), 0 = vanilla. Writes a one-line reason. */
static int leave_decide(unsigned char* mp, void* key, char* why, size_t whysz) {
    const char* kp = ""; uint32_t kl = 0;
    if (!key_text(key, &kp, &kl)) { kp = "?"; kl = 1; }
    unsigned char* list = *(unsigned char**)(mp + g_off_list);
    unsigned char* sp   = *(unsigned char**)(mp + g_off_setup);
    void* setup = sp ? *(void**)(sp + g_off_setup_sub) : NULL;
    if (!list || !setup) { _snprintf_s(why, whysz, _TRUNCATE, "faction=%.*s: no player list/setup -> vanilla", (int)kl, kp); return 0; }

    /* local player (this machine is the session authority when the original would send) */
    int local_on = 0; uint32_t local_idx = 0xFFFFFFFFu;
    unsigned char* la = *(unsigned char**)(mp + g_off_loc_a);
    unsigned char* lb = la ? *(unsigned char**)(la + g_off_loc_b) : NULL;
    if (lb) {
        uint32_t id = *(uint32_t*)(lb + g_off_loc_c);
        if (id < NP_MAX) {
            local_idx = *(uint32_t*)(list + g_off_map + id * 4);
            unsigned char* rec = (unsigned char*)((fn_recat_t)g_leave_recat)(list, local_idx);
            if (rec && controls_faction(rec) && key_eq(((fn_getkey_t)g_leave_getkey)(rec, setup), key)) local_on = 1;
        }
    }
    /* every other slot: same key object (the leaving player's own setup entry) vs equal key text (another entry) */
    uint32_t cnt = *(uint32_t*)(list + NP_COUNT_OFF), same_ptr = 0, others = 0, ctrl = 0;
    if (cnt > NP_MAX) cnt = NP_MAX;
    for (uint32_t i = 0; i < cnt; ++i) {
        unsigned char* rec = (unsigned char*)((fn_recat_t)g_leave_recat)(list, i);
        if (!rec || !controls_faction(rec)) continue;
        ++ctrl;
        void* k = ((fn_getkey_t)g_leave_getkey)(rec, setup);
        if (k == key) ++same_ptr; else if (key_eq(k, key)) ++others;
    }
    int keep = local_on || others >= 1 || same_ptr >= 2;
    _snprintf_s(why, whysz, _TRUNCATE,
        "faction=%.*s controllers=%u local_slot=%d local_on_faction=%d same_entry=%u other_entries=%u -> %s",
        (int)kl, kp, ctrl, local_idx == 0xFFFFFFFFu ? -1 : (int)local_idx, local_on, same_ptr, others,
        keep ? "KEEP HUMAN (switch-to-AI suppressed)" : "vanilla (switch-to-AI sent)");
    return keep;
}

static uint64_t hook_send_switch(void* mp, void* key) {
    char why[200]; int keep = 0;
    InterlockedIncrement(&g_leave_calls);
    __try { keep = leave_decide((unsigned char*)mp, key, why, sizeof why); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        keep = 0; InterlockedIncrement(&g_leave_errors);
        _snprintf_s(why, sizeof why, _TRUNCATE, "exception 0x%08lx in decision -> vanilla", GetExceptionCode());
    }
    if (keep) InterlockedIncrement(&g_leave_skipped); else InterlockedIncrement(&g_leave_passed);
    EnterCriticalSection(&g_last_lock);
    strncpy_s(g_leave_last, sizeof g_leave_last, why, _TRUNCATE);
    LeaveCriticalSection(&g_last_lock);
    logline("leave hook #%ld: %s", g_leave_calls, why);
    if (keep) return 0;
    return ((fn_send_t)g_tramp)(mp, key);
}

static int build_trampoline(void) {
    if (g_tramp) return 1;
    unsigned char* t = (unsigned char*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!t) return 0;
    uint64_t back = (uint64_t)(g_leave_send + 16);
    memcpy(t, SEND_PROLOGUE, 16);                                   /* = the original bytes (verified)    */
    t[16] = 0xFF; t[17] = 0x25; memset(t + 18, 0, 4);               /* jmp qword [rip+0]                  */
    memcpy(t + 22, &back, 8);
    DWORD old;
    if (!VirtualProtect(t, 4096, PAGE_EXECUTE_READ, &old)) { VirtualFree(t, 0, MEM_RELEASE); return 0; }
    FlushInstructionCache(GetCurrentProcess(), t, 30);
    g_tramp = t;
    return 1;
}

/* =================================================================================================== */
/* Patch E: the hook. Runs where the engine seats a player in a resumed lobby (the host: its own seat in
 * hosted_game, then every joiner's ADD_FILLED_PLAYER_SLOT request).                                        */

static int e_key_text(const void* s, const char** p, uint32_t* len) {
    if (!s) return 0;
    *len = ((fn_strlen_t)g_res_strlen)((void*)s);
    *p   = ((fn_cstr_t)g_res_cstr)((void*)s);
    if (*len > 255 || (!*p && *len)) return 0;
    return 1;
}
static int e_key_eq(const void* a, const void* b) {
    const char *pa, *pb; uint32_t la, lb;
    if (a == b) return 1;
    if (!e_key_text(a, &pa, &la) || !e_key_text(b, &pb, &lb)) return 0;
    return la == lb && (la == 0 || memcmp(pa, pb, la) == 0);
}

/* Returns the key to seat the joiner on (a pointer into R's own key array, like the original), or NULL to
 * let the original decide. Rule: the joiner's own tuple (account id match) wins while the number of seated
 * slots holding its faction is below the number of saved tuples for that faction. *shared = 1 when vanilla
 * would have refused it (the faction already had a seated player).                                         */
static void* resume_decide(unsigned char* R, const uint64_t* id, const unsigned char* taken, int* shared,
                           char* why, size_t whysz) {
    *shared = 0;
    if (!R || !id || !taken) { _snprintf_s(why, whysz, _TRUNCATE, "null argument -> vanilla"); return NULL; }
    uint32_t n    = R[R_COUNT_U8];
    uint32_t cnt  = *(const uint32_t*)(R + R_KEYCOUNT);
    uint32_t tcnt = *(const uint32_t*)(taken + TAKEN_COUNT);
    const unsigned char* keys = *(unsigned char* const*)(R + R_KEYS_PTR);
    const uint64_t*      ids  = *(const uint64_t* const*)(R + R_IDS_PTR);
    const unsigned char* tk   = *(unsigned char* const*)(taken + TAKEN_PTR);
    if (!keys || !ids || n == 0 || cnt == 0) { _snprintf_s(why, whysz, _TRUNCATE, "account %llu: no saved players -> vanilla", (unsigned long long)*id); return NULL; }
    if (n > 64 || cnt > 64 || tcnt > 64 || (tcnt && !tk)) {
        _snprintf_s(why, whysz, _TRUNCATE, "implausible sizes (players %u keys %u seated %u) -> vanilla", n, cnt, tcnt); return NULL;
    }
    uint32_t mm = n < cnt ? n : cnt, full_tuple = 0xFFFFFFFFu, fcap = 0, fused = 0;
    for (uint32_t i = 0; i < mm; ++i) {
        if (ids[i] != *id) continue;
        const unsigned char* key = keys + (size_t)i * KEY_STRIDE;
        uint32_t cap = 0, used = 0;
        for (uint32_t j = 0; j < cnt; ++j)  if (e_key_eq(keys + (size_t)j * KEY_STRIDE, key)) ++cap;
        for (uint32_t t = 0; t < tcnt; ++t) if (e_key_eq(tk + (size_t)t * KEY_STRIDE, key)) ++used;
        if (used < cap) {
            const char* kp = "?"; uint32_t kl = 1; e_key_text(key, &kp, &kl);
            *shared = used > 0;
            _snprintf_s(why, whysz, _TRUNCATE, "account %llu tuple %u faction=%.*s saved_tuples=%u seated=%u -> OWN SAVED FACTION%s",
                        (unsigned long long)*id, i, (int)kl, kp, cap, used,
                        used > 0 ? " (shared seat; vanilla would refuse it)" : " (same as vanilla)");
            return (void*)key;
        }
        full_tuple = i; fcap = cap; fused = used;
    }
    if (full_tuple != 0xFFFFFFFFu)
        _snprintf_s(why, whysz, _TRUNCATE, "account %llu: own faction full (tuple %u saved_tuples=%u seated=%u) -> vanilla",
                    (unsigned long long)*id, full_tuple, fcap, fused);
    else
        _snprintf_s(why, whysz, _TRUNCATE, "account %llu: not in the save (%u tuples) -> vanilla", (unsigned long long)*id, mm);
    return NULL;
}

static void* hook_pick(void* R, uint64_t* id, void* taken) {
    char why[240]; void* res = NULL; int shared = 0;
    InterlockedIncrement(&g_res_calls);
    __try { res = resume_decide((unsigned char*)R, id, (const unsigned char*)taken, &shared, why, sizeof why); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        res = NULL; shared = 0; InterlockedIncrement(&g_res_errors);
        _snprintf_s(why, sizeof why, _TRUNCATE, "exception 0x%08lx in decision -> vanilla", GetExceptionCode());
    }
    if (res && shared) InterlockedIncrement(&g_res_shared);
    if (!res) InterlockedIncrement(&g_res_vanilla);
    EnterCriticalSection(&g_last_lock);
    strncpy_s(g_res_last, sizeof g_res_last, why, _TRUNCATE);
    LeaveCriticalSection(&g_last_lock);
    logline("resume hook #%ld: %s", g_res_calls, why);
    if (res) return res;
    return ((fn_pick_t)g_tramp_res)(R, id, taken);
}

static int build_trampoline_res(void) {
    if (g_tramp_res) return 1;
    unsigned char* t = (unsigned char*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!t) return 0;
    uint64_t back = (uint64_t)(g_res_pick + 16);
    memcpy(t, PICK_PROLOGUE, 16);                                   /* = the original bytes (verified)    */
    t[16] = 0xFF; t[17] = 0x25; memset(t + 18, 0, 4);               /* jmp qword [rip+0]                  */
    memcpy(t + 22, &back, 8);
    DWORD old;
    if (!VirtualProtect(t, 4096, PAGE_EXECUTE_READ, &old)) { VirtualFree(t, 0, MEM_RELEASE); return 0; }
    FlushInstructionCache(GetCurrentProcess(), t, 30);
    g_tramp_res = t;
    return 1;
}

/* =================================================================================================== */
/* Patch F: the hooks. Both run on the UI path of the local machine only; nothing here reaches the model.    */

/* returns the faction the local machine would end the turn for, NULL when it is not that faction's turn */
static void* et_local_if_turn(unsigned char* env) {
    unsigned char* ui    = *(unsigned char**)(env + ET_OFF_UI);
    unsigned char* model = *(unsigned char**)(ui + ET_OFF_MODEL);
    void*          fac   = *(void**)(ui + ET_OFF_LOCAL);
    unsigned char* world = *(unsigned char**)(model + ET_OFF_WORLD);
    void*          etm   = *(void**)(world + ET_OFF_ETM);
    return fac && ((fn_isturn_t)g_et_isturn)(etm, fac) ? fac : NULL;
}

static void hook_issue(unsigned char* env, unsigned char flag) {
    void* fac = NULL;
    InterlockedIncrement(&g_et_calls);
    if (g_et_shared && !g_et_bypass && !flag) {
        __try { fac = et_local_if_turn(env); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            fac = NULL; InterlockedIncrement(&g_et_errors);
            logline("endturn hook: exception 0x%08lx -> vanilla", GetExceptionCode());
        }
    }
    if (fac) {
        g_et_env = env;
        if (!g_et_view) {
            g_et_view = fac; InterlockedIncrement(&g_et_presses);
            logline("endturn hook #%ld: END_TURN held -> vote (faction 0x%llx)", g_et_calls, (unsigned long long)(uintptr_t)fac);
        }
        return;
    }
    InterlockedIncrement(&g_et_passed);
    logline("endturn hook #%ld: END_TURN passed (flag=%u shared=%ld bypass=%ld)", g_et_calls, flag, g_et_shared, g_et_bypass);
    ((fn_issue_t)(g_et_page + ET_PAGE_ISSUER))(env, flag);
}

static void hook_cancel(unsigned char* env) {
    if (g_et_shared && g_et_view) {
        g_et_view = NULL; InterlockedIncrement(&g_et_cancels);
        logline("endturn hook: CANCEL %s", g_et_bypass ? "passed (vote already complete)" : "held -> vote withdrawn");
        if (!g_et_bypass) return;
    }
    ((fn_cancel_t)(g_et_page + ET_PAGE_CANCEL))(env);
}

/* a page within rel32 reach of the image: just above it */
static unsigned char* alloc_near(void) {
    const uintptr_t step = 0x10000;
    uintptr_t base = (g_imgbase + nt_of(GetModuleHandleW(NULL))->OptionalHeader.SizeOfImage + step - 1) & ~(step - 1);
    unsigned char* t = NULL;
    for (uintptr_t a = base; a < base + 0x40000000 && !t; a += step)
        t = (unsigned char*)VirtualAlloc((LPVOID)a, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!t) return NULL;
    if ((uintptr_t)t < g_imgbase || (uintptr_t)t - g_imgbase > 0x7FF00000u) { VirtualFree(t, 0, MEM_RELEASE); return NULL; }
    return t;
}

/* Holds the thunk and both trampolines. */
static int build_et_page(void) {
    if (g_et_page) return 1;
    unsigned char* t = alloc_near();
    if (!t) return 0;

    /* thunk(array, &faction): answers true for the pressed faction, else tail-jumps into find. Touches rax only.
     *   mov rax,&g_et_view; mov rax,[rax]; test rax,rax; je orig; cmp rax,[rdx]; jne orig; mov al,1; ret
     *   orig: jmp qword [rip+0] <find>                                                                       */
    static const unsigned char head[] = { 0x48,0x8B,0x00, 0x48,0x85,0xC0, 0x74,0x08, 0x48,0x3B,0x02, 0x75,0x03, 0xB0,0x01, 0xC3,
                                          0xFF,0x25,0x00,0x00,0x00,0x00 };
    uint64_t view = (uint64_t)(uintptr_t)&g_et_view, find = (uint64_t)g_et_find;
    t[0] = 0x48; t[1] = 0xB8; memcpy(t + 2, &view, 8);
    memcpy(t + 10, head, sizeof head);
    memcpy(t + 10 + sizeof head, &find, 8);

    struct { int off; const unsigned char* bytes; uintptr_t target; } tr[2] = {
        { ET_PAGE_ISSUER, ET_ISSUER_PROLOGUE, g_et_issuer }, { ET_PAGE_CANCEL, ET_CANCEL_PROLOGUE, g_et_cancel } };
    for (int i = 0; i < 2; ++i) {
        unsigned char* p = t + tr[i].off; uint64_t back = (uint64_t)(tr[i].target + 18);
        memcpy(p, tr[i].bytes, 18);
        p[18] = 0xFF; p[19] = 0x25; memset(p + 20, 0, 4);           /* jmp qword [rip+0]                  */
        memcpy(p + 24, &back, 8);
    }
    DWORD old;
    if (!VirtualProtect(t, 4096, PAGE_EXECUTE_READ, &old)) { VirtualFree(t, 0, MEM_RELEASE); return 0; }
    FlushInstructionCache(GetCurrentProcess(), t, 4096);
    g_et_page = t;
    return 1;
}

/* =================================================================================================== */
/* Patch G: the gift system on a shared army. Local UI only, except the two gift sends it asks for.        */

/* a slot that holds an army and is not a spectator: the walk of 0x1421b518c */
static unsigned char* gf_slot(unsigned char* players, uint32_t i) {
    if (i >= *(uint32_t*)(players + GF_PL_COUNT)) return NULL;
    unsigned char* s = players + (size_t)i * GF_SLOT_STRIDE;
    uint32_t f = *(uint32_t*)(s + GF_SLOT_FLAGS);
    return (f & 0x13) == 1 && (f & 0x25) != 0x25 ? s : NULL;
}

static int gf_same_army(const unsigned char* a, const unsigned char* b) {
    return *(int*)(a + GF_SLOT_ALLIANCE) == *(int*)(b + GF_SLOT_ALLIANCE) && *(int*)(a + GF_SLOT_ARMY) == *(int*)(b + GF_SLOT_ARMY);
}

/* the local slot, through the same two virtual calls the game makes; NULL outside a multiplayer battle */
static unsigned char* gf_me(unsigned char** players, uint32_t* local, unsigned char** mp_out) {
    unsigned char* env   = *(unsigned char**)g_gf_env;
    unsigned char* iface = env ? *(unsigned char**)(env + GF_ENV_IFACE) : NULL;
    unsigned char* mp    = iface ? *(unsigned char**)(iface + GF_IFACE_MP) : NULL;
    if (!mp) return NULL;
    unsigned char* o = mp + 0x18;
    unsigned char* n = (unsigned char*)((*(fn_vget_t**)o)[0x1c0 / 8])(o);
    if (!n) return NULL;
    *local   = ((*(fn_vslot_t**)n)[0x20 / 8])(n);
    *players = *(unsigned char**)(mp + GF_MP_PLAYERS);
    if (mp_out) *mp_out = mp;
    return gf_slot(*players, *local);
}

static int gf_holds(const unsigned char* slot, int id) {
    uint32_t n = *(uint32_t*)(slot + GF_SLOT_LIST_N);
    unsigned char* e = *(unsigned char**)(slot + GF_SLOT_LIST_P);
    for (uint32_t i = 0; i < n; ++i, e += GF_ENTRY_STRIDE) {
        unsigned char* low = *(unsigned char**)(e + GF_ENTRY_LOW);
        if (low && *(int*)(low + GF_LOW_ID) == id) return 1;
    }
    return 0;
}

/* the other players on the local army: their slots, how many */
static int gf_others(uint32_t* out) {
    unsigned char* players; uint32_t local; int n = 0;
    unsigned char* me = gf_me(&players, &local, NULL);
    if (!me) return 0;
    uint32_t count = *(uint32_t*)(players + GF_PL_COUNT);
    for (uint32_t i = 0; i < count && n < GF_MAX_SLOTS; ++i) {
        unsigned char* s = i == local ? NULL : gf_slot(players, i);
        if (s && gf_same_army(me, s)) out[n++] = i;
    }
    return n;
}

/* Stands in for "first slot of this army" where the UI asks who originally owned a unit, so the answer is per unit
 * and per machine. A player who lost a unit is its original owner on his own machine, whichever slot he is in: that
 * is what keeps his card and shows it greyed, with the recall button. A player who has it alone sees it as gifted
 * from the others. A unit everyone holds, or nobody holds, keeps the game's answer. */
static uint32_t gf_original(void* local_player, uint32_t alliance, uint32_t army) {
    uint32_t first = ((fn_first_t)g_gf_first)(local_player, alliance, army);
    __try {
        unsigned char* ctx = g_gf_ctx;
        unsigned char* unit = ctx ? *(unsigned char**)(ctx + 8) : NULL;
        unsigned char* players; uint32_t local; uint32_t others[GF_MAX_SLOTS];
        unsigned char* me = unit ? gf_me(&players, &local, NULL) : NULL;
        if (!me || *(uint32_t*)(me + GF_SLOT_ALLIANCE) != alliance || *(uint32_t*)(me + GF_SLOT_ARMY) != army) return first;
        int n = gf_others(others), id = *(int*)(unit + GF_UNIT_ID), someone = 0, lacks = -1;
        for (int i = 0; i < n; ++i) {
            if (gf_holds(gf_slot(players, others[i]), id)) someone = 1;
            else if (lacks < 0) lacks = (int)others[i];
        }
        InterlockedIncrement(&g_gf_answers);
        if (!gf_holds(me, id)) return someone ? local : first;
        return lacks >= 0 ? (uint32_t)lacks : first;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_gf_errors) <= 5) logline("gift: exception 0x%08lx in the owner answer -> vanilla", GetExceptionCode());
    }
    return first;
}

/* GiftUnits with the local player's own row: take the selection from the others. The receiver would ignore a gift
 * from a slot to itself, so the row sends other -> local, the same message a recall sends. */
static void gf_gift(void* env, uint32_t from, uint32_t to) {
    uint32_t others[GF_MAX_SLOTS]; int n = 0;
    if (from == to) {
        __try { n = gf_others(others); }
        __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; InterlockedIncrement(&g_gf_errors); }
    }
    if (n == 0) { ((fn_gift_sel_t)g_gf_sel)(env, from, to); return; }
    logline("gift: slot %u takes the selection from %d other player(s)", to, n);
    for (int i = 0; i < n; ++i) ((fn_gift_sel_t)g_gf_sel)(env, others[i], to);
}

/* The recall button on a shared army gives the unit back to everyone: the holder keeps his copy. */
static void gf_recall(void* env, uint32_t from, uint32_t to, void* units) {
    uint32_t others[GF_MAX_SLOTS]; int n = 0;
    __try { n = gf_others(others); }
    __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; InterlockedIncrement(&g_gf_errors); }
    g_gf_keep = n > 0;
    ((fn_gift_units_t)g_gf_units)(env, from, to, units);
    g_gf_keep = 0;
}

static unsigned char gf_send(void* mp, uint32_t from, uint32_t to, void* entry, unsigned char keep) {
    if (g_gf_keep) keep = 1;
    logline("gift send: slot %u -> %u, keep=%u", from, to, keep);
    return ((fn_gift_send_t)g_gf_send)(mp, from, to, entry, keep);
}

/* The popup's list; nonzero makes the stub clear the slot the popup would skip, so the local player gets a row. */
static int gf_players(void* players, uint32_t alliance, void* out) {
    uint32_t others[GF_MAX_SLOTS]; int n = 0;
    ((fn_gift_list_t)g_gf_list)(players, alliance, out);
    __try { n = gf_others(others); }
    __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; InterlockedIncrement(&g_gf_errors); }
    return n > 0;
}

/* jmp qword [rip+0] <to> */
static void gf_stub(unsigned char* p, void* to) {
    uint64_t dst = (uint64_t)(uintptr_t)to;
    p[0] = 0xFF; p[1] = 0x25; memset(p + 2, 0, 4); memcpy(p + 6, &dst, 8);
}

static int build_gf_page(void) {
    if (g_gf_page) return 1;
    unsigned char* t = alloc_near();
    if (!t) return 0;
    /* army_of(context): remember the context, then the real function. Touches r10 only.
     *   mov r10,&g_gf_ctx; mov [r10],rcx; jmp qword [rip+0] <army_of>                                        */
    uint64_t ctx = (uint64_t)(uintptr_t)&g_gf_ctx;
    t[0] = 0x49; t[1] = 0xBA; memcpy(t + 2, &ctx, 8);
    t[10] = 0x49; t[11] = 0x89; t[12] = 0x0A;
    gf_stub(t + 13, (void*)g_gf_army);
    gf_stub(t + GF_PAGE_OFF[GF_S_ORIG_FIRST], (void*)&gf_original);
    gf_stub(t + GF_PAGE_OFF[GF_S_GIFT],       (void*)&gf_gift);
    gf_stub(t + GF_PAGE_OFF[GF_S_RECALL],     (void*)&gf_recall);
    gf_stub(t + GF_PAGE_OFF[GF_S_SEND],       (void*)&gf_send);
    /* sub rsp,0x28; call [rip+0x14] <gf_players>; add rsp,0x28; test eax,eax; je ret;
     * or r15d,-1; mov dword [rbp+0x48],-1 (the caller reloads r15d from there); ret; <gf_players>             */
    static const unsigned char list[] = { 0x48,0x83,0xEC,0x28, 0xFF,0x15,0x14,0x00,0x00,0x00, 0x48,0x83,0xC4,0x28, 0x85,0xC0, 0x74,0x0B,
                                          0x41,0x83,0xCF,0xFF, 0xC7,0x45,0x48,0xFF,0xFF,0xFF,0xFF, 0xC3 };
    uint64_t fn = (uint64_t)(uintptr_t)&gf_players;
    unsigned char* p = t + GF_PAGE_OFF[GF_S_LIST];
    memcpy(p, list, sizeof list);
    memcpy(p + sizeof list, &fn, 8);
    DWORD old;
    if (!VirtualProtect(t, 4096, PAGE_EXECUTE_READ, &old)) { VirtualFree(t, 0, MEM_RELEASE); return 0; }
    FlushInstructionCache(GetCurrentProcess(), t, 4096);
    g_gf_page = t;
    return 1;
}

/* =================================================================================================== */
/* Lua-facing functions                                                                                */

static void set_va(lua_State* L, const char* field, uintptr_t va) {
    char h[32]; sprintf_s(h, sizeof h, "0x%llx", (unsigned long long)va); push_str(L, h); A.setfield(L, -2, field);
}

static int l_status(lua_State* L) {
    A.createtable(L, 0, 16);
    A.pushboolean(L, g_host_ok);                     A.setfield(L, -2, "supported");
    A.pushboolean(L, g_exe_tested);                  A.setfield(L, -2, "exe_tested");
    push_str(L, g_reason);                           A.setfield(L, -2, "reason");
    push_str(L, ARCHON_VERSION);                     A.setfield(L, -2, "version");
    set_va(L, "canset_va",        g_canset);
    set_va(L, "readybit_va",      g_bt);
    set_va(L, "dupstring_va",     g_dupstr);
    set_va(L, "lea_va",           g_lea);
    set_va(L, "producer_va",      g_producer);
    set_va(L, "producer_cont_va", g_prod_cont);
    A.pushboolean(L, g_patch_canset);                A.setfield(L, -2, "patch_canset");
    A.pushboolean(L, g_patch_ready);                 A.setfield(L, -2, "patch_ready");
    A.pushboolean(L, g_patch_producer);              A.setfield(L, -2, "patch_producer");
    /* patch D */
    A.pushboolean(L, g_leave_ok);                    A.setfield(L, -2, "leave_supported");
    push_str(L, g_leave_reason);                     A.setfield(L, -2, "leave_reason");
    A.pushboolean(L, g_patch_leave);                 A.setfield(L, -2, "patch_leave");
    set_va(L, "leave_send_va",    g_leave_send);
    set_va(L, "leave_handler_va", g_leave_handler);
    set_va(L, "leave_setidle_va", g_leave_setidle);
    A.pushinteger(L, (ptrdiff_t)g_leave_calls);      A.setfield(L, -2, "leave_calls");
    A.pushinteger(L, (ptrdiff_t)g_leave_skipped);    A.setfield(L, -2, "leave_kept_human");
    A.pushinteger(L, (ptrdiff_t)g_leave_passed);     A.setfield(L, -2, "leave_vanilla");
    A.pushinteger(L, (ptrdiff_t)g_leave_errors);     A.setfield(L, -2, "leave_errors");
    { char last[256];
      EnterCriticalSection(&g_last_lock); strncpy_s(last, sizeof last, g_leave_last, _TRUNCATE); LeaveCriticalSection(&g_last_lock);
      push_str(L, last); A.setfield(L, -2, "leave_last"); }
    /* patch E */
    A.pushboolean(L, g_res_ok);                      A.setfield(L, -2, "resume_supported");
    push_str(L, g_res_reason);                       A.setfield(L, -2, "resume_reason");
    A.pushboolean(L, g_patch_res);                   A.setfield(L, -2, "patch_resume");
    set_va(L, "resume_pick_va",   g_res_pick);
    A.pushinteger(L, (ptrdiff_t)g_res_calls);        A.setfield(L, -2, "resume_calls");
    A.pushinteger(L, (ptrdiff_t)g_res_shared);       A.setfield(L, -2, "resume_shared");
    A.pushinteger(L, (ptrdiff_t)g_res_vanilla);      A.setfield(L, -2, "resume_vanilla");
    A.pushinteger(L, (ptrdiff_t)g_res_errors);       A.setfield(L, -2, "resume_errors");
    { char last[256];
      EnterCriticalSection(&g_last_lock); strncpy_s(last, sizeof last, g_res_last, _TRUNCATE); LeaveCriticalSection(&g_last_lock);
      push_str(L, last); A.setfield(L, -2, "resume_last"); }
    /* patch F */
    A.pushboolean(L, g_et_ok);                       A.setfield(L, -2, "endturn_supported");
    push_str(L, g_et_reason);                        A.setfield(L, -2, "endturn_reason");
    A.pushboolean(L, g_patch_et);                    A.setfield(L, -2, "patch_endturn");
    set_va(L, "endturn_issuer_va", g_et_issuer);
    set_va(L, "endturn_cancel_va", g_et_cancel);
    A.pushinteger(L, (ptrdiff_t)g_et_calls);         A.setfield(L, -2, "endturn_calls");
    A.pushinteger(L, (ptrdiff_t)g_et_presses);       A.setfield(L, -2, "endturn_presses");
    A.pushinteger(L, (ptrdiff_t)g_et_passed);        A.setfield(L, -2, "endturn_passed");
    A.pushinteger(L, (ptrdiff_t)g_et_cancels);       A.setfield(L, -2, "endturn_cancels");
    A.pushinteger(L, (ptrdiff_t)g_et_errors);        A.setfield(L, -2, "endturn_errors");
    /* patch G */
    A.pushboolean(L, g_gf_ok);                       A.setfield(L, -2, "gift_supported");
    push_str(L, g_gf_reason);                        A.setfield(L, -2, "gift_reason");
    A.pushboolean(L, g_patch_gf);                    A.setfield(L, -2, "patch_gift");
    A.pushinteger(L, (ptrdiff_t)g_gf_answers);       A.setfield(L, -2, "gift_answers");
    A.pushinteger(L, (ptrdiff_t)g_gf_errors);        A.setfield(L, -2, "gift_errors");
    return 1;
}

/* Patch G: the eight retargets, all or nothing (idempotent). Applied at boot in every context: each C function above
 * falls through to the game's own unless another player shares the local army, which only Archon produces. */
static int l_patch_gift(lua_State* L) {
    EnterCriticalSection(&g_lock);
    if (!g_host_ok || !g_gf_ok) { LeaveCriticalSection(&g_lock); push_str(L, "refused: patch G target unresolved"); return 1; }
    if (g_patch_gf) { LeaveCriticalSection(&g_lock); push_str(L, "ok (already applied)"); return 1; }
    int same = 1;
    for (int i = 0; i < GF_SITES; ++i)
        same = same && *(const unsigned char*)g_gf_site[i] == 0xE8 && rel32_target(g_gf_site[i], 1, 5) == g_gf_want[i];
    if (!same) {
        LeaveCriticalSection(&g_lock); logline("patch_gift REFUSED: a target no longer holds its original bytes");
        push_str(L, "refused: target bytes changed since init"); return 1;
    }
    if (!build_gf_page()) { LeaveCriticalSection(&g_lock); logline("patch_gift FAILED: near page allocation"); push_str(L, "error: near page allocation failed"); return 1; }

    int ok = 1, sites = 0;
    for (; ok && sites < GF_SITES; ++sites) {
        unsigned char call[5]; int32_t rel = (int32_t)((intptr_t)(g_gf_page + GF_PAGE_OFF[sites]) - (intptr_t)(g_gf_site[sites] + 5));
        call[0] = 0xE8; memcpy(call + 1, &rel, 4);
        ok = write_bytes(g_gf_site[sites], call, 5, g_gf_save_site[sites]);
        if (!ok) break;
    }
    if (!ok) {
        while (--sites >= 0) write_bytes(g_gf_site[sites], g_gf_save_site[sites], 5, NULL);
        LeaveCriticalSection(&g_lock); logline("patch_gift FAILED (VirtualProtect/write); rolled back");
        push_str(L, "error: patch write failed (VirtualProtect)"); return 1;
    }
    g_patch_gf = 1;
    logline("patch_gift applied: %d calls -> page 0x%llx", GF_SITES, (unsigned long long)(uintptr_t)g_gf_page);
    LeaveCriticalSection(&g_lock);
    push_str(L, "ok"); return 1;
}

/* Patch F: the two hooks + the five retargets, all or nothing (idempotent). */
static int l_patch_endturn(lua_State* L) {
    EnterCriticalSection(&g_lock);
    if (!g_host_ok || !g_et_ok) { LeaveCriticalSection(&g_lock); push_str(L, "refused: patch F target unresolved"); return 1; }
    if (g_patch_et) { LeaveCriticalSection(&g_lock); push_str(L, "ok (already applied)"); return 1; }
    int same = memcmp((void*)g_et_issuer, ET_ISSUER_PROLOGUE, 18) == 0 && memcmp((void*)g_et_cancel, ET_CANCEL_PROLOGUE, 18) == 0;
    for (int i = 0; i < ET_SITES; ++i)
        same = same && *(const unsigned char*)g_et_site[i] == 0xE8 && rel32_target(g_et_site[i], 1, 5) == g_et_find;
    if (!same) {
        LeaveCriticalSection(&g_lock); logline("patch_endturn REFUSED: a target no longer holds its original bytes");
        push_str(L, "refused: target bytes changed since init"); return 1;
    }
    if (!build_et_page()) { LeaveCriticalSection(&g_lock); logline("patch_endturn FAILED: near page allocation"); push_str(L, "error: near page allocation failed"); return 1; }

    int ok = 1, sites = 0, issuer = 0, cancel = 0;
    for (; ok && sites < ET_SITES; ++sites) {
        unsigned char call[5]; int32_t rel = (int32_t)((intptr_t)g_et_page - (intptr_t)(g_et_site[sites] + 5));
        call[0] = 0xE8; memcpy(call + 1, &rel, 4);
        ok = write_bytes(g_et_site[sites], call, 5, g_et_save_site[sites]);
        if (!ok) break;
    }
    unsigned char jmp[16]; uint64_t dst;
    jmp[0] = 0xFF; jmp[1] = 0x25; memset(jmp + 2, 0, 4); jmp[14] = 0xCC; jmp[15] = 0xCC;
    if (ok) { dst = (uint64_t)(uintptr_t)&hook_issue;  memcpy(jmp + 6, &dst, 8); ok = issuer = write_bytes(g_et_issuer, jmp, 16, g_et_save_issuer); }
    if (ok) { dst = (uint64_t)(uintptr_t)&hook_cancel; memcpy(jmp + 6, &dst, 8); ok = cancel = write_bytes(g_et_cancel, jmp, 16, g_et_save_cancel); }
    if (!ok) {
        if (issuer) write_bytes(g_et_issuer, g_et_save_issuer, 16, NULL);
        while (--sites >= 0) write_bytes(g_et_site[sites], g_et_save_site[sites], 5, NULL);
        LeaveCriticalSection(&g_lock); logline("patch_endturn FAILED (VirtualProtect/write); rolled back");
        push_str(L, "error: patch write failed (VirtualProtect)"); return 1;
    }
    g_patch_et = 1;
    logline("patch_endturn applied: issuer 0x%llx, cancel 0x%llx -> [FF 25 00000000 <hook> CC CC]; %d readers -> thunk 0x%llx",
            (unsigned long long)g_et_issuer, (unsigned long long)g_et_cancel, ET_SITES, (unsigned long long)(uintptr_t)g_et_page);
    LeaveCriticalSection(&g_lock);
    push_str(L, "ok"); return 1;
}

static int l_unpatch_endturn(lua_State* L) {
    EnterCriticalSection(&g_lock);
    int ok = 1;
    if (g_patch_et) {
        ok &= write_bytes(g_et_cancel, g_et_save_cancel, 16, NULL);
        ok &= write_bytes(g_et_issuer, g_et_save_issuer, 16, NULL);
        for (int i = 0; i < ET_SITES; ++i) ok &= write_bytes(g_et_site[i], g_et_save_site[i], 5, NULL);
        if (ok) g_patch_et = 0;
    }
    LeaveCriticalSection(&g_lock);              /* the near page stays allocated: a thread may still be in it */
    logline("unpatch_endturn: %s", ok ? "restored original bytes" : "restore had a write error");
    push_str(L, ok ? "ok" : "error: restore write failed");
    return 1;
}

static int l_endturn_set_shared(lua_State* L) { g_et_shared = A.toboolean(L, 1) ? 1 : 0; return 0; }
static int l_endturn_bypass(lua_State* L)     { g_et_bypass = A.toboolean(L, 1) ? 1 : 0; return 0; }
static int l_endturn_pressed(lua_State* L)    { A.pushboolean(L, g_et_view != NULL); return 1; }
static int l_endturn_reset(lua_State* L) {
    (void)L; g_et_view = NULL; g_et_bypass = 0; g_et_env = NULL;
    return 0;
}

/* Consensus reached on this machine: one vanilla end turn through the CampaignRoot.EndTurn executor (it applies the
 * gates, then calls the hooked issuer, in bypass). Returns true when the issuer was reached, false when a gate refused. */
static int l_endturn_issue(lua_State* L) {
    LONG before = g_et_passed; int ok = 0;
    g_et_bypass = 1;
    if (g_patch_et) {
        __try { ((fn_exec_t)g_et_exec)(); ok = g_et_passed != before; }
        __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_et_errors); logline("endturn_issue: exception 0x%08lx", GetExceptionCode()); }
    }
    A.pushboolean(L, ok); return 1;
}

/* A cancel crossed the completion: withdraw the real end turn too (vanilla cancel, no-op unless the faction is ready) */
static int l_endturn_cancel(lua_State* L) {
    (void)L;
    if (g_patch_et && g_et_env) ((fn_cancel_t)(g_et_page + ET_PAGE_CANCEL))(g_et_env);
    return 0;
}

/* the engine's own answer for the local faction, whatever this machine pressed */
static int l_endturn_real_ready(lua_State* L) {
    int ready = 0; unsigned char* env = g_et_env;
    if (env) {
        __try {
            unsigned char* ui    = *(unsigned char**)(env + ET_OFF_UI);
            unsigned char* model = *(unsigned char**)(ui + ET_OFF_MODEL);
            void*          fac   = *(void**)(ui + ET_OFF_LOCAL);
            unsigned char* world = *(unsigned char**)(model + ET_OFF_WORLD);
            unsigned char* etm   = *(unsigned char**)(world + ET_OFF_ETM);
            unsigned char* R     = *(unsigned char**)(etm + ET_OFF_R);
            ready = ((fn_find_t)g_et_find)(R + ET_OFF_READY, &fac) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_et_errors); }
    }
    A.pushboolean(L, ready); return 1;
}

/* Patch E: install the 16-byte absolute jmp at the picker's entry (idempotent). */
static int l_patch_resume(lua_State* L) {
    EnterCriticalSection(&g_lock);
    if (!g_host_ok || !g_res_ok) { LeaveCriticalSection(&g_lock); push_str(L, "refused: patch E target unresolved"); return 1; }
    if (g_patch_res) { LeaveCriticalSection(&g_lock); push_str(L, "ok (already applied)"); return 1; }
    if (memcmp((void*)g_res_pick, PICK_PROLOGUE, 16) != 0) {
        LeaveCriticalSection(&g_lock); logline("patch_resume REFUSED: picker no longer holds its original bytes");
        push_str(L, "refused: target bytes changed since init"); return 1;
    }
    if (!build_trampoline_res()) { LeaveCriticalSection(&g_lock); logline("patch_resume FAILED: trampoline allocation"); push_str(L, "error: trampoline allocation failed"); return 1; }
    unsigned char jmp[16];
    uint64_t dst = (uint64_t)(uintptr_t)&hook_pick;
    jmp[0] = 0xFF; jmp[1] = 0x25; memset(jmp + 2, 0, 4); memcpy(jmp + 6, &dst, 8); jmp[14] = 0xCC; jmp[15] = 0xCC;
    if (!write_bytes(g_res_pick, jmp, 16, g_save_res)) {
        LeaveCriticalSection(&g_lock); logline("patch_resume FAILED (VirtualProtect/write)"); push_str(L, "error: patch write failed (VirtualProtect)"); return 1;
    }
    g_patch_res = 1;
    logline("patch_resume applied: picker 0x%llx [48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57] -> [FF 25 00000000 <0x%llx> CC CC], "
            "trampoline 0x%llx (host seating: join 0x%llx via wrapper 0x%llx)",
            (unsigned long long)g_res_pick, (unsigned long long)dst, (unsigned long long)(uintptr_t)g_tramp_res,
            (unsigned long long)g_res_join, (unsigned long long)g_res_wrap);
    LeaveCriticalSection(&g_lock);
    push_str(L, "ok"); return 1;
}

static int l_unpatch_resume(lua_State* L) {
    EnterCriticalSection(&g_lock);
    int ok = 1;
    if (g_patch_res) { ok = write_bytes(g_res_pick, g_save_res, 16, NULL); if (ok) g_patch_res = 0; }
    LeaveCriticalSection(&g_lock);              /* the trampoline stays allocated: a thread may still be in it */
    logline("unpatch_resume: %s", ok ? "restored original bytes" : "restore had a write error");
    push_str(L, ok ? "ok" : "error: restore write failed");
    return 1;
}

/* Patch D: install the 16-byte absolute jmp at the sender's entry (idempotent). */
static int l_patch_leave(lua_State* L) {
    EnterCriticalSection(&g_lock);
    if (!g_host_ok || !g_leave_ok) { LeaveCriticalSection(&g_lock); push_str(L, "refused: patch D target unresolved"); return 1; }
    if (g_patch_leave) { LeaveCriticalSection(&g_lock); push_str(L, "ok (already applied)"); return 1; }
    if (memcmp((void*)g_leave_send, SEND_PROLOGUE, 16) != 0) {
        LeaveCriticalSection(&g_lock); logline("patch_leave REFUSED: sender no longer holds its original bytes");
        push_str(L, "refused: target bytes changed since init"); return 1;
    }
    if (!build_trampoline()) { LeaveCriticalSection(&g_lock); logline("patch_leave FAILED: trampoline allocation"); push_str(L, "error: trampoline allocation failed"); return 1; }
    unsigned char jmp[16];
    uint64_t dst = (uint64_t)(uintptr_t)&hook_send_switch;
    jmp[0] = 0xFF; jmp[1] = 0x25; memset(jmp + 2, 0, 4); memcpy(jmp + 6, &dst, 8); jmp[14] = 0xCC; jmp[15] = 0xCC;
    if (!write_bytes(g_leave_send, jmp, 16, g_save_leave)) {
        LeaveCriticalSection(&g_lock); logline("patch_leave FAILED (VirtualProtect/write)"); push_str(L, "error: patch write failed (VirtualProtect)"); return 1;
    }
    g_patch_leave = 1;
    logline("patch_leave applied: sender 0x%llx [48 89 5C 24 08 57 48 83 EC 30 48 8B D9 48 8B FA] -> [FF 25 00000000 <0x%llx> CC CC], "
            "trampoline 0x%llx (CCQ_FACTION_SWITCH_HUMAN_TO_AI executor 0x%llx -> set_idle_human 0x%llx)",
            (unsigned long long)g_leave_send, (unsigned long long)dst, (unsigned long long)(uintptr_t)g_tramp,
            (unsigned long long)g_leave_handler, (unsigned long long)g_leave_setidle);
    LeaveCriticalSection(&g_lock);
    push_str(L, "ok"); return 1;
}

static int l_unpatch_leave(lua_State* L) {
    EnterCriticalSection(&g_lock);
    int ok = 1;
    if (g_patch_leave) { ok = write_bytes(g_leave_send, g_save_leave, 16, NULL); if (ok) g_patch_leave = 0; }
    LeaveCriticalSection(&g_lock);              /* the trampoline stays allocated: a thread may still be in it */
    logline("unpatch_leave: %s", ok ? "restored original bytes" : "restore had a write error");
    push_str(L, ok ? "ok" : "error: restore write failed");
    return 1;
}

/* Applies A+B+C atomically: either all three end up applied, or none that this call touched. */
static int l_patch_lobby(lua_State* L) {
    EnterCriticalSection(&g_lock);
    if (!g_host_ok) { LeaveCriticalSection(&g_lock); push_str(L, "refused: host not supported / targets unresolved"); return 1; }
    if (g_patch_canset && g_patch_ready && g_patch_producer) { LeaveCriticalSection(&g_lock); push_str(L, "ok (already applied)"); return 1; }

    /* pre-write verification: each unapplied site must still hold its original bytes */
    if ((!g_patch_canset   && memcmp((void*)g_canset,   CANSET_EXPECT, 3) != 0) ||
        (!g_patch_ready    && memcmp((void*)g_bt,       BT_EXPECT,     5) != 0) ||
        (!g_patch_producer && memcmp((void*)g_producer, g_prod_orig,   6) != 0)) {
        LeaveCriticalSection(&g_lock);
        logline("patch_lobby REFUSED: a target no longer holds its original bytes");
        push_str(L, "refused: target bytes changed since init"); return 1;
    }

    int a_new = 0, b_new = 0, c_new = 0, ok = 1;
    if (ok && !g_patch_canset)   { ok = write_bytes(g_canset,   CANSET_NEW, 3, g_save_canset);   a_new = ok; }
    if (ok && !g_patch_ready)    { ok = write_bytes(g_bt,       BT_NEW,     5, g_save_ready);    b_new = ok; }
    if (ok && !g_patch_producer) { ok = write_bytes(g_producer, g_prod_new, 6, g_save_producer); c_new = ok; }
    if (ok) {
        char o[18], nw[18]; hex6(o, g_save_producer); hex6(nw, g_prod_new);
        g_patch_canset = g_patch_ready = g_patch_producer = 1;
        logline("patch_lobby applied: canset 0x%llx [%02X %02X %02X]->[B0 01 C3]; readybit 0x%llx [48 0F BA E0 1C]->[F8 90 90 90 90]; "
                "producer 0x%llx [%s]->[%s] (jne 0x%llx -> 0x%llx)",
                (unsigned long long)g_canset, g_save_canset[0], g_save_canset[1], g_save_canset[2],
                (unsigned long long)g_bt, (unsigned long long)g_producer, o, nw,
                (unsigned long long)g_prod_dup, (unsigned long long)g_prod_cont);
        LeaveCriticalSection(&g_lock); push_str(L, "ok"); return 1;
    }
    /* partial failure: roll back whatever THIS call applied */
    if (c_new) write_bytes(g_producer, g_save_producer, 6, NULL);
    if (b_new) write_bytes(g_bt,       g_save_ready,    5, NULL);
    if (a_new) write_bytes(g_canset,   g_save_canset,   3, NULL);
    LeaveCriticalSection(&g_lock);
    logline("patch_lobby FAILED (VirtualProtect/write); rolled back");
    push_str(L, "error: patch write failed (VirtualProtect)"); return 1;
}

static int l_unpatch_lobby(lua_State* L) {
    EnterCriticalSection(&g_lock);
    int ok = 1;
    if (g_patch_producer) { ok &= write_bytes(g_producer, g_save_producer, 6, NULL); g_patch_producer = 0; }
    if (g_patch_ready)    { ok &= write_bytes(g_bt,       g_save_ready,    5, NULL); g_patch_ready = 0; }
    if (g_patch_canset)   { ok &= write_bytes(g_canset,   g_save_canset,   3, NULL); g_patch_canset = 0; }
    LeaveCriticalSection(&g_lock);
    logline("unpatch_lobby: %s", ok ? "restored original bytes" : "restore had a write error");
    push_str(L, ok ? "ok" : "error: restore write failed");
    return 1;
}

/* =================================================================================================== */
__declspec(dllexport) int luaopen_archon_native(lua_State* L) {
    __try {
        if (!g_init_done) {
            g_init_done = 1;
            InitializeCriticalSection(&g_lock);
            InitializeCriticalSection(&g_last_lock);
            do_init();
        }
        A.createtable(L, 0, 16);
        reg_fn(L, "status",        l_status);
        reg_fn(L, "patch_lobby",   l_patch_lobby);
        reg_fn(L, "unpatch_lobby", l_unpatch_lobby);
        reg_fn(L, "patch_leave",   l_patch_leave);
        reg_fn(L, "unpatch_leave", l_unpatch_leave);
        reg_fn(L, "patch_resume",  l_patch_resume);
        reg_fn(L, "unpatch_resume",l_unpatch_resume);
        reg_fn(L, "patch_endturn",      l_patch_endturn);
        reg_fn(L, "unpatch_endturn",    l_unpatch_endturn);
        reg_fn(L, "endturn_set_shared", l_endturn_set_shared);
        reg_fn(L, "endturn_bypass",     l_endturn_bypass);
        reg_fn(L, "endturn_pressed",    l_endturn_pressed);
        reg_fn(L, "endturn_reset",      l_endturn_reset);
        reg_fn(L, "endturn_issue",      l_endturn_issue);
        reg_fn(L, "endturn_cancel",     l_endturn_cancel);
        reg_fn(L, "endturn_real_ready", l_endturn_real_ready);
        reg_fn(L, "patch_gift",         l_patch_gift);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logline("luaopen_archon_native: exception 0x%08lx (returning empty table)", GetExceptionCode());
    }
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
