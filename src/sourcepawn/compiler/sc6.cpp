// vim: set sts=2 ts=8 sw=2 tw=99 et:
/*  Pawn compiler - Binary code generation (the "assembler")
 *
 *  Copyright (c) ITB CompuPhase, 1997-2006
 *
 *  This software is provided "as-is", without any express or implied warranty.
 *  In no event will the authors be held liable for any damages arising from
 *  the use of this software.
 *
 *  Permission is granted to anyone to use this software for any purpose,
 *  including commercial applications, and to alter it and redistribute it
 *  freely, subject to the following restrictions:
 *
 *  1.  The origin of this software must not be misrepresented; you must not
 *      claim that you wrote the original software. If you use this software in
 *      a product, an acknowledgment in the product documentation would be
 *      appreciated but is not required.
 *  2.  Altered source versions must be plainly marked as such, and must not be
 *      misrepresented as being the original software.
 *  3.  This notice may not be removed or altered from any source distribution.
 *
 *  Version: $Id$
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>     /* for macro max() */
#include <stddef.h>     /* for macro offsetof() */
#include <string.h>
#include <ctype.h>
#if defined FORTIFY
  #include <alloc/fortify.h>
#endif
#include "lstring.h"
#include "sc.h"
#include "amxdbg.h"
#if defined LINUX || defined __FreeBSD__ || defined __OpenBSD__
  #include "sclinux.h"
#endif
#include <am-string.h>
#include <smx/smx-v1.h>
#include <smx/smx-v1-opcodes.h>
#include <zlib/zlib.h>
#include "smx-builder.h"
#include "memory-buffer.h"
#include "types.h"

using namespace sp;
using namespace ke;

class CellWriter
{
 public:
  explicit CellWriter(Vector<cell>* buffer)
   : buffer_(buffer),
     current_index_(0)
  {}

  void append(cell value) {
    if (buffer_) {
      buffer_->append(value);
    }
    current_index_ += sizeof(value);
  }

  cell current_index() const {
    return current_index_;
  }

 private:
  Vector<cell>* buffer_;
  cell current_index_;
};

typedef void (*OPCODE_PROC)(CellWriter* writer, char *params, cell opcode);

typedef struct {
  cell opcode;
  const char *name;
  int segment;          /* sIN_CSEG=parse in cseg, sIN_DSEG=parse in dseg */
  OPCODE_PROC func;
} OPCODEC;

static cell *LabelTable;    /* label table */

/* apparently, strtol() does not work correctly on very large (unsigned)
 * hexadecimal values */
static ucell hex2long(const char *s,char **n)
{
  ucell result=0L;
  int negate=FALSE;
  int digit;

  /* ignore leading whitespace */
  while (*s==' ' || *s=='\t')
    s++;

  /* allow a negation sign to create the two's complement of numbers */
  if (*s=='-') {
    negate=TRUE;
    s++;
  } /* if */

  assert((*s>='0' && *s<='9') || (*s>='a' && *s<='f') || (*s>='a' && *s<='f'));
  for ( ;; ) {
    if (*s>='0' && *s<='9')
      digit=*s-'0';
    else if (*s>='a' && *s<='f')
      digit=*s-'a' + 10;
    else if (*s>='A' && *s<='F')
      digit=*s-'A' + 10;
    else
      break;    /* probably whitespace */
    result=(result<<4) | digit;
    s++;
  } /* for */
  if (n!=NULL)
    *n=(char*)s;
  if (negate)
    result=(~result)+1; /* take two's complement of the result */
  return (ucell)result;
}

static ucell getparam(const char *s,char **n)
{
  ucell result=0;
  for ( ;; ) {
    result+=hex2long(s,(char**)&s);
    if (*s!='+')
      break;
    s++;
  } /* for */
  if (n!=NULL)
    *n=(char*)s;
  return result;
}

static char *skipwhitespace(char *str)
{
  while (isspace(*str))
    str++;
  return str;
}

static char *stripcomment(char *str)
{
  char *ptr=strchr(str,';');
  if (ptr!=NULL) {
    *ptr++='\n';        /* terminate the line, but leave the '\n' */
    *ptr='\0';
  } /* if */
  return str;
}

static void noop(CellWriter* writer, char *params, cell opcode)
{
}

static void set_currentfile(CellWriter* writer, char *params, cell opcode)
{
  fcurrent=(short)getparam(params,NULL);
}

static void parm0(CellWriter* writer, char *params, cell opcode)
{
  writer->append(opcode);
}

static void parm1(CellWriter* writer, char *params, cell opcode)
{
  ucell p = getparam(params, nullptr);
  writer->append(opcode);
  writer->append(p);
}

static void parm2(CellWriter* writer, char *params, cell opcode)
{
  ucell p1 = getparam(params, &params);
  ucell p2 = getparam(params, nullptr);
  writer->append(opcode);
  writer->append(p1);
  writer->append(p2);
}

static void parm3(CellWriter* writer, char *params, cell opcode)
{
  ucell p1 = getparam(params, &params);
  ucell p2 = getparam(params, &params);
  ucell p3 = getparam(params, nullptr);
  writer->append(opcode);
  writer->append(p1);
  writer->append(p2);
  writer->append(p3);
}

static void parm4(CellWriter* writer, char *params, cell opcode)
{
  ucell p1 = getparam(params, &params);
  ucell p2 = getparam(params, &params);
  ucell p3 = getparam(params, &params);
  ucell p4 = getparam(params, nullptr);
  writer->append(opcode);
  writer->append(p1);
  writer->append(p2);
  writer->append(p3);
  writer->append(p4);
}

static void parm5(CellWriter* writer, char *params, cell opcode)
{
  ucell p1 = getparam(params, &params);
  ucell p2 = getparam(params, &params);
  ucell p3 = getparam(params, &params);
  ucell p4 = getparam(params, &params);
  ucell p5 = getparam(params, nullptr);
  writer->append(opcode);
  writer->append(p1);
  writer->append(p2);
  writer->append(p3);
  writer->append(p4);
  writer->append(p5);
}

static void do_dump(CellWriter* writer, char *params, cell opcode)
{
  int num = 0;

  while (*params != '\0') {
    ucell p = getparam(params, &params);
    writer->append(p);
    num++;
    while (isspace(*params))
      params++;
  }
}

static symbol*
extract_call_target(char *params)
{
  char name[METHOD_NAMEMAX];

  int i;
  for (i=0; !isspace(*params); i++,params++) {
    assert(*params != '\0');
    assert(i < METHOD_NAMEMAX);
    name[i] = *params;
  }
  name[i]='\0';

  symbol* sym = findglb(name);
  if (!sym) {
    return nullptr;
  }

  assert(sym->ident == iFUNCTN);
  assert(sym->vclass == sGLOBAL);
  return sym;
}

static void do_ldgfen(CellWriter* writer, char *params, cell opcode)
{
  symbol *sym = extract_call_target(params);
  assert(sym->ident == iFUNCTN);
  assert(!(sym->usage & uNATIVE));
  assert((sym->funcid & 1) == 1);

  // Note: we emit const.pri for backward compatibility.
  assert(opcode == sp::OP_UNGEN_LDGFN_PRI);
  writer->append(sp::OP_CONST_PRI);
  writer->append(sym->funcid);
}

static void do_call(CellWriter* writer, char *params, cell opcode)
{
  symbol* sym = extract_call_target(params);

  writer->append(opcode);
  writer->append(sym->addr());
}

static void do_jump(CellWriter* writer, char *params, cell opcode)
{
  int i = (int)hex2long(params, nullptr);
  assert(i >= 0 && i < sc_labnum);

  writer->append(opcode);
  writer->append(LabelTable[i]);
}

static void do_switch(CellWriter* writer, char *params, cell opcode)
{
  int i = (int)hex2long(params, nullptr);
  assert(i >= 0 && i < sc_labnum);

  writer->append(opcode);
  writer->append(LabelTable[i]);
}

static void do_case(CellWriter* writer, char *params, cell opcode)
{
  cell v = hex2long(params ,&params);
  int i = (int)hex2long(params, nullptr);
  assert(i >= 0 && i < sc_labnum);

  writer->append(v);
  writer->append(LabelTable[i]);
}

static OPCODEC opcodelist[] = {
  /* node for "invalid instruction" */
  {  0, NULL,         0,        noop },
  /* opcodes in sorted order */
  { 78, "add",        sIN_CSEG, parm0 },
  { 87, "add.c",      sIN_CSEG, parm1 },
  { 14, "addr.alt",   sIN_CSEG, parm1 },
  { 13, "addr.pri",   sIN_CSEG, parm1 },
  { 81, "and",        sIN_CSEG, parm0 },
  {121, "bounds",     sIN_CSEG, parm1 },
  {137, "break",      sIN_CSEG, parm0 },  /* version 8 */
  { 49, "call",       sIN_CSEG, do_call },
  {  0, "case",       sIN_CSEG, do_case },
  {130, "casetbl",    sIN_CSEG, parm0 },  /* version 1 */
  {  0, "code",       sIN_CSEG, set_currentfile },
  {156, "const",      sIN_CSEG, parm2 },  /* version 9 */
  { 12, "const.alt",  sIN_CSEG, parm1 },
  { 11, "const.pri",  sIN_CSEG, parm1 },
  {157, "const.s",    sIN_CSEG, parm2 },  /* version 9 */
  {  0, "data",       sIN_DSEG, set_currentfile },
  {114, "dec",        sIN_CSEG, parm1 },
  {113, "dec.alt",    sIN_CSEG, parm0 },
  {116, "dec.i",      sIN_CSEG, parm0 },
  {112, "dec.pri",    sIN_CSEG, parm0 },
  {115, "dec.s",      sIN_CSEG, parm1 },
  {  0, "dump",       sIN_DSEG, do_dump },
  {166, "endproc",    sIN_CSEG, parm0 },
  { 95, "eq",         sIN_CSEG, parm0 },
  {106, "eq.c.alt",   sIN_CSEG, parm1 },
  {105, "eq.c.pri",   sIN_CSEG, parm1 },
  {119, "fill",       sIN_CSEG, parm1 },
  {162, "genarray",   sIN_CSEG, parm1 },
  {163, "genarray.z", sIN_CSEG, parm1 },
  {120, "halt",       sIN_CSEG, parm1 },
  { 45, "heap",       sIN_CSEG, parm1 },
  { 27, "idxaddr",    sIN_CSEG, parm0 },
  { 28, "idxaddr.b",  sIN_CSEG, parm1 },
  {109, "inc",        sIN_CSEG, parm1 },
  {108, "inc.alt",    sIN_CSEG, parm0 },
  {111, "inc.i",      sIN_CSEG, parm0 },
  {107, "inc.pri",    sIN_CSEG, parm0 },
  {110, "inc.s",      sIN_CSEG, parm1 },
  { 86, "invert",     sIN_CSEG, parm0 },
  { 55, "jeq",        sIN_CSEG, do_jump },
  { 56, "jneq",       sIN_CSEG, do_jump },
  { 54, "jnz",        sIN_CSEG, do_jump },
  { 64, "jsgeq",      sIN_CSEG, do_jump },
  { 63, "jsgrtr",     sIN_CSEG, do_jump },
  { 62, "jsleq",      sIN_CSEG, do_jump },
  { 61, "jsless",     sIN_CSEG, do_jump },
  { 51, "jump",       sIN_CSEG, do_jump },
  { 53, "jzer",       sIN_CSEG, do_jump },
  {167, "ldgfn.pri",  sIN_CSEG, do_ldgfen },
  { 25, "lidx",       sIN_CSEG, parm0 },
  { 26, "lidx.b",     sIN_CSEG, parm1 },
  {  2, "load.alt",   sIN_CSEG, parm1 },
  {154, "load.both",  sIN_CSEG, parm2 },  /* version 9 */
  {  9, "load.i",     sIN_CSEG, parm0 },
  {  1, "load.pri",   sIN_CSEG, parm1 },
  {  4, "load.s.alt", sIN_CSEG, parm1 },
  {155, "load.s.both",sIN_CSEG, parm2 },  /* version 9 */
  {  3, "load.s.pri", sIN_CSEG, parm1 },
  { 10, "lodb.i",     sIN_CSEG, parm1 },
  {  8, "lref.s.alt", sIN_CSEG, parm1 },
  {  7, "lref.s.pri", sIN_CSEG, parm1 },
  { 34, "move.alt",   sIN_CSEG, parm0 },
  { 33, "move.pri",   sIN_CSEG, parm0 },
  {117, "movs",       sIN_CSEG, parm1 },
  { 85, "neg",        sIN_CSEG, parm0 },
  { 96, "neq",        sIN_CSEG, parm0 },
  {134, "nop",        sIN_CSEG, parm0 },  /* version 6 */
  { 84, "not",        sIN_CSEG, parm0 },
  { 82, "or",         sIN_CSEG, parm0 },
  { 43, "pop.alt",    sIN_CSEG, parm0 },
  { 42, "pop.pri",    sIN_CSEG, parm0 },
  { 46, "proc",       sIN_CSEG, parm0 },
  { 40, "push",       sIN_CSEG, parm1 },
  {133, "push.adr",   sIN_CSEG, parm1 },  /* version 4 */
  { 37, "push.alt",   sIN_CSEG, parm0 },
  { 39, "push.c",     sIN_CSEG, parm1 },
  { 36, "push.pri",   sIN_CSEG, parm0 },
  { 41, "push.s",     sIN_CSEG, parm1 },
  {139, "push2",      sIN_CSEG, parm2 },  /* version 9 */
  {141, "push2.adr",  sIN_CSEG, parm2 },  /* version 9 */
  {138, "push2.c",    sIN_CSEG, parm2 },  /* version 9 */
  {140, "push2.s",    sIN_CSEG, parm2 },  /* version 9 */
  {143, "push3",      sIN_CSEG, parm3 },  /* version 9 */
  {145, "push3.adr",  sIN_CSEG, parm3 },  /* version 9 */
  {142, "push3.c",    sIN_CSEG, parm3 },  /* version 9 */
  {144, "push3.s",    sIN_CSEG, parm3 },  /* version 9 */
  {147, "push4",      sIN_CSEG, parm4 },  /* version 9 */
  {149, "push4.adr",  sIN_CSEG, parm4 },  /* version 9 */
  {146, "push4.c",    sIN_CSEG, parm4 },  /* version 9 */
  {148, "push4.s",    sIN_CSEG, parm4 },  /* version 9 */
  {151, "push5",      sIN_CSEG, parm5 },  /* version 9 */
  {153, "push5.adr",  sIN_CSEG, parm5 },  /* version 9 */
  {150, "push5.c",    sIN_CSEG, parm5 },  /* version 9 */
  {152, "push5.s",    sIN_CSEG, parm5 },  /* version 9 */
  { 48, "retn",       sIN_CSEG, parm0 },
  { 74, "sdiv.alt",   sIN_CSEG, parm0 },
  {104, "sgeq",       sIN_CSEG, parm0 },
  {103, "sgrtr",      sIN_CSEG, parm0 },
  { 65, "shl",        sIN_CSEG, parm0 },
  { 69, "shl.c.alt",  sIN_CSEG, parm1 },
  { 68, "shl.c.pri",  sIN_CSEG, parm1 },
  { 66, "shr",        sIN_CSEG, parm0 },
  { 71, "shr.c.alt",  sIN_CSEG, parm1 },
  { 70, "shr.c.pri",  sIN_CSEG, parm1 },
  {102, "sleq",       sIN_CSEG, parm0 },
  {101, "sless",      sIN_CSEG, parm0 },
  { 72, "smul",       sIN_CSEG, parm0 },
  { 88, "smul.c",     sIN_CSEG, parm1 },
  { 22, "sref.s.alt", sIN_CSEG, parm1 },
  { 21, "sref.s.pri", sIN_CSEG, parm1 },
  { 67, "sshr",       sIN_CSEG, parm0 },
  { 44, "stack",      sIN_CSEG, parm1 },
  {  0, "stksize",    0,        noop },
  { 16, "stor.alt",   sIN_CSEG, parm1 },
  { 23, "stor.i",     sIN_CSEG, parm0 },
  { 15, "stor.pri",   sIN_CSEG, parm1 },
  { 18, "stor.s.alt", sIN_CSEG, parm1 },
  { 17, "stor.s.pri", sIN_CSEG, parm1 },
  {164, "stradjust.pri", sIN_CSEG, parm0 },
  { 24, "strb.i",     sIN_CSEG, parm1 },
  { 79, "sub",        sIN_CSEG, parm0 },
  { 80, "sub.alt",    sIN_CSEG, parm0 },
  {132, "swap.alt",   sIN_CSEG, parm0 },  /* version 4 */
  {131, "swap.pri",   sIN_CSEG, parm0 },  /* version 4 */
  {129, "switch",     sIN_CSEG, do_switch }, /* version 1 */
  {135, "sysreq.n",   sIN_CSEG, parm2 },  /* version 9 (replaces SYSREQ.d from earlier version) */
  {161, "tracker.pop.setheap", sIN_CSEG, parm0 },
  {160, "tracker.push.c", sIN_CSEG, parm1 },
  { 35, "xchg",       sIN_CSEG, parm0 },
  { 83, "xor",        sIN_CSEG, parm0 },
  { 91, "zero",       sIN_CSEG, parm1 },
  { 90, "zero.alt",   sIN_CSEG, parm0 },
  { 89, "zero.pri",   sIN_CSEG, parm0 },
  { 92, "zero.s",     sIN_CSEG, parm1 },
};

#define MAX_INSTR_LEN   30
static int findopcode(char *instr,int maxlen)
{
  int low,high,mid,cmp;
  char str[MAX_INSTR_LEN];

  if (maxlen>=MAX_INSTR_LEN)
    return 0;
  strlcpy(str,instr,maxlen+1);
  /* look up the instruction with a binary search
   * the assembler is case insensitive to instructions (but case sensitive
   * to symbols)
   */
  low=1;                /* entry 0 is reserved (for "not found") */
  high=(sizeof opcodelist / sizeof opcodelist[0])-1;
  while (low<high) {
    mid=(low+high)/2;
    assert(opcodelist[mid].name!=NULL);
    cmp=stricmp(str,opcodelist[mid].name);
    if (cmp>0)
      low=mid+1;
    else
      high=mid;
  } /* while */

  assert(low==high);
  if (stricmp(str,opcodelist[low].name)==0)
    return low;         /* found */
  return 0;             /* not found, return special index */
}

// This pass is necessary because the code addresses of labels is only known
// after the peephole optimization flag. Labels can occur inside expressions
// (e.g. the conditional operator), which are optimized.
static void relocate_labels(void *fin)
{
  if (sc_labnum <= 0)
    return;

  assert(!LabelTable);
  LabelTable = (cell *)calloc(sc_labnum, sizeof(cell));

  char line[256];

  CellWriter writer(nullptr);

  pc_resetasm(fin);
  while (pc_readasm(fin, line, sizeof(line))) {
    stripcomment(line);

    char *instr = skipwhitespace(line);
    if (*instr == '\0') // Ignore empty lines.
      continue;

    if (tolower(*instr) == 'l' && *(instr + 1) == '.') {
      int lindex = (int)hex2long(instr + 2, nullptr);
      assert(lindex >= 0 && lindex < sc_labnum);
      LabelTable[lindex] = writer.current_index();
    } else {
      // Get to the end of the instruction (make use of the '\n' that fgets()
      // added at the end of the line; this way we *always* drop on a whitespace
      // character.
      char *params;
      for (params = instr; *params != '\0' && !isspace(*params); params++) {
        // Nothing.
      }
      assert(params > instr);

      int op_index = findopcode(instr, (int)(params - instr));
      OPCODEC &op = opcodelist[op_index];
      if (!op.name) {
        *params = '\0';
        error(104, instr);
      }

      if (op.segment == sIN_CSEG)
        op.func(&writer, skipwhitespace(params), op.opcode);
    }
  }
}

// Generate code or data into a buffer.
static void generate_segment(Vector<cell> *buffer, void *fin, int pass)
{
  pc_resetasm(fin);

  char line[255];
  while (pc_readasm(fin, line, sizeof(line))) {
    stripcomment(line);
    char *instr = skipwhitespace(line);
    
    // Ignore empty lines and labels.
    if (*instr=='\0' || (tolower(*instr) == 'l' && *(instr + 1)=='.'))
      continue;

    // Get to the end of the instruction (make use of the '\n' that fgets()
    // added at the end of the line; this way we will *always* drop on a
    // whitespace character) */
    char *params;
    for (params=instr; *params != '\0' && !isspace(*params); params++) {
      // Do nothing.
    }
    assert(params > instr);

    int op_index = findopcode(instr, (int)(params-instr));
    OPCODEC &op = opcodelist[op_index];
    assert(op.name != nullptr);

    if (op.segment != pass)
      continue;

    CellWriter writer(buffer);
    op.func(&writer, skipwhitespace(params), op.opcode);
  }
}

#if !defined NDEBUG
// The opcode list should be sorted by name.
class VerifyOpcodeSorting
{
 public:
  VerifyOpcodeSorting() {
    assert(opcodelist[1].name!=NULL);
    for (size_t i = 2; i<(sizeof opcodelist / sizeof opcodelist[0]); i++) {
      assert(opcodelist[i].name!=NULL);
      assert(stricmp(opcodelist[i].name,opcodelist[i-1].name)>0);
    } /* for */
  }
} sVerifyOpcodeSorting;
#endif

static int sort_by_addr(const void *a1, const void *a2)
{
  symbol *s1 = *(symbol **)a1;
  symbol *s2 = *(symbol **)a2;
  return s1->addr() - s2->addr();
}

struct function_entry {
  symbol *sym;
  AString name;
};

static int sort_functions(const void *a1, const void *a2)
{
  function_entry &f1 = *(function_entry *)a1;
  function_entry &f2 = *(function_entry *)a2;
  return strcmp(f1.name.chars(), f2.name.chars());
}

// Helper for parsing a debug string. Debug strings look like this:
//  L:40 10
class DebugString
{
 public:
  DebugString() : kind_('\0'), str_(nullptr)
  { }
  DebugString(char *str)
   : kind_(str[0]),
     str_(str)
  {
    assert(str_[1] == ':');
    str_ += 2;
  }
  char kind() const {
    return kind_;
  }
  ucell parse() {
    return hex2long(str_, &str_);
  }
  char *skipspaces() {
    str_ = ::skipwhitespace(str_);
    return str_;
  }
  void expect(char c) {
    assert(*str_ == c);
    str_++;
  }
  char *skipto(char c) {
    str_ = strchr(str_, c);
    return str_;
  }
  char getc() {
    return *str_++;
  }

 private:
  char kind_;
  char *str_;
};

typedef SmxBlobSection<sp_fdbg_info_t> SmxDebugInfoSection;
typedef SmxListSection<sp_fdbg_line_t> SmxDebugLineSection;
typedef SmxListSection<sp_fdbg_file_t> SmxDebugFileSection;
typedef SmxListSection<sp_file_tag_t> SmxTagSection;
typedef SmxBlobSection<void> SmxDebugSymbolsSection;
typedef SmxBlobSection<void> SmxDebugNativesSection;
typedef Vector<symbol *> SymbolList;

static void append_debug_tables(SmxBuilder *builder, StringPool &pool, RefPtr<SmxNameTable> names, SymbolList &nativeList)
{
  // We use a separate name table for historical reasons that are no longer
  // necessary. In the future we should just alias this to ".names".
  RefPtr<SmxNameTable> dbgnames = new SmxNameTable(".dbg.strings");
  RefPtr<SmxDebugInfoSection> info = new SmxDebugInfoSection(".dbg.info");
  RefPtr<SmxDebugLineSection> lines = new SmxDebugLineSection(".dbg.lines");
  RefPtr<SmxDebugFileSection> files = new SmxDebugFileSection(".dbg.files");
  RefPtr<SmxDebugSymbolsSection> symbols = new SmxDebugSymbolsSection(".dbg.symbols");
  RefPtr<SmxDebugNativesSection> natives = new SmxDebugNativesSection(".dbg.natives");
  RefPtr<SmxTagSection> tags = new SmxTagSection(".tags");

  stringlist *dbgstrs = get_dbgstrings();

  // State for tracking which file we're on. We replicate the original AMXDBG
  // behavior here which excludes duplicate addresses.
  ucell prev_file_addr = 0;
  const char *prev_file_name = nullptr;

  // Add debug data.
  for (stringlist *iter = dbgstrs; iter; iter = iter->next) {
    if (iter->line[0] == '\0')
      continue;

    DebugString str(iter->line);
    switch (str.kind()) {
      case 'F':
      {
        ucell codeidx = str.parse();
        if (codeidx != prev_file_addr) {
          if (prev_file_name) {
            sp_fdbg_file_t &entry = files->add();
            entry.addr = prev_file_addr;
            entry.name = dbgnames->add(pool, prev_file_name);
          }
          prev_file_addr = codeidx;
        }
        prev_file_name = str.skipspaces();
        break;
      }

      case 'L':
      {
        sp_fdbg_line_t &entry = lines->add();
        entry.addr = str.parse();
        entry.line = str.parse();
        break;
      }

      case 'S':
      {
        sp_fdbg_symbol_t sym;
        sp_fdbg_arraydim_t dims[sDIMEN_MAX];

        sym.addr = str.parse();
        sym.tagid = str.parse();

        str.skipspaces();
        str.expect(':');
        char *name = str.skipspaces();
        char *nameend = str.skipto(' ');
        Atom *atom = pool.add(name, nameend - name);

        sym.codestart = str.parse();
        sym.codeend = str.parse();
        sym.ident = (char)str.parse();
        sym.vclass = (char)str.parse();
        sym.dimcount = 0;
        sym.name = dbgnames->add(atom);

        info->header().num_syms++;

        str.skipspaces();
        if (str.getc() == '[') {
          info->header().num_arrays++;
          for (char *ptr = str.skipspaces(); *ptr != ']'; ptr = str.skipspaces()) {
            dims[sym.dimcount].tagid = str.parse();
            str.skipspaces();
            str.expect(':');
            dims[sym.dimcount].size = str.parse();
            sym.dimcount++;
          }
        }

        symbols->add(&sym, sizeof(sym));
        symbols->add(dims, sizeof(dims[0]) * sym.dimcount);
        break;
      }
    }
  }

  // Add the last file.
  if (prev_file_name) {
    sp_fdbg_file_t &entry = files->add();
    entry.addr = prev_file_addr;
    entry.name = dbgnames->add(pool, prev_file_name);
  }

  // Build the tags table.
  gTypes.forEachType([&](Type* type) -> void {
    assert(strlen(type->name())>0);

    sp_file_tag_t &tag = tags->add();
    tag.tag_id = type->smx_export_value();
    tag.name = names->add(pool, type->name());
  });

  // Finish up debug header statistics.
  info->header().num_files = files->count();
  info->header().num_lines = lines->count();

  // Write natives.
  sp_fdbg_ntvtab_t natives_header;
  natives_header.num_entries = nativeList.length();
  natives->add(&natives_header, sizeof(natives_header));

  for (size_t i = 0; i < nativeList.length(); i++) {
    symbol *sym = nativeList[i];

    sp_fdbg_native_t info;
    info.index = i;
    info.name = dbgnames->add(pool, sym->name);
    info.tagid = sym->tag;
    info.nargs = 0;
    for (arginfo *arg = sym->dim.arglist; arg->ident; arg++)
      info.nargs++;
    natives->add(&info, sizeof(info));

    for (arginfo *arg = sym->dim.arglist; arg->ident; arg++) {
      sp_fdbg_ntvarg_t argout;
      argout.ident = arg->ident;
      argout.tagid = arg->tag;
      argout.dimcount = arg->numdim;
      argout.name = dbgnames->add(pool, arg->name);
      natives->add(&argout, sizeof(argout));

      for (int j = 0; j < argout.dimcount; j++) {
        sp_fdbg_arraydim_t dim;
        dim.tagid = arg->idxtag[j];
        dim.size = arg->dim[j];
        natives->add(&dim, sizeof(dim));
      }
    }
  }

  // Add these in the same order SourceMod 1.6 added them.
  builder->add(files);
  builder->add(symbols);
  builder->add(lines);
  builder->add(natives);
  builder->add(dbgnames);
  builder->add(info);
  builder->add(tags);
}

typedef SmxListSection<sp_file_natives_t> SmxNativeSection;
typedef SmxListSection<sp_file_publics_t> SmxPublicSection;
typedef SmxListSection<sp_file_pubvars_t> SmxPubvarSection;
typedef SmxBlobSection<sp_file_data_t> SmxDataSection;
typedef SmxBlobSection<sp_file_code_t> SmxCodeSection;

static void assemble_to_buffer(MemoryBuffer *buffer, void *fin)
{
  StringPool pool;
  SmxBuilder builder;
  RefPtr<SmxNativeSection> natives = new SmxNativeSection(".natives");
  RefPtr<SmxPublicSection> publics = new SmxPublicSection(".publics");
  RefPtr<SmxPubvarSection> pubvars = new SmxPubvarSection(".pubvars");
  RefPtr<SmxDataSection> data = new SmxDataSection(".data");
  RefPtr<SmxCodeSection> code = new SmxCodeSection(".code");
  RefPtr<SmxNameTable> names = new SmxNameTable(".names");

  Vector<symbol *> nativeList;
  Vector<function_entry> functions;

  // Build the easy symbol tables.
  for (symbol *sym=glbtab.next; sym; sym=sym->next) {
    if (sym->ident==iFUNCTN) {
      if ((sym->usage & uNATIVE)!=0 && (sym->usage & uREAD)!=0 && sym->addr() >= 0) {
        // Natives require special handling, so we save them for later.
        nativeList.append(sym);
        continue;
      }

      // If a function is marked as missing it should not be a public function
      // with a declaration.
      if (sym->usage & uMISSING) {
        assert((sym->usage & (uPUBLIC|uDEFINE)) != (uPUBLIC|uDEFINE));
        continue;
      }
      
      if ((sym->usage & (uPUBLIC|uDEFINE)) == (uPUBLIC|uDEFINE) ||
          (sym->usage & uREAD))
      {
        function_entry entry;
        entry.sym = sym;
        if (sym->usage & uPUBLIC) {
          entry.name = sym->name;
        } else {
          // Create a private name.
          char private_name[sNAMEMAX*3 + 1];
          snprintf(private_name, sizeof(private_name), ".%d.%s", sym->addr(), sym->name);

          entry.name = private_name;
        }

        functions.append(entry);
        continue;
      }
    } else if (sym->ident==iVARIABLE || sym->ident == iARRAY || sym->ident == iREFARRAY) {
      if ((sym->usage & uPUBLIC)!=0 && (sym->usage & (uREAD | uWRITTEN))!=0) {
        sp_file_pubvars_t &pubvar = pubvars->add();
        pubvar.address = sym->addr();
        pubvar.name = names->add(pool, sym->name);
      }
    }
  }

  // The public list must be sorted.
  qsort(functions.buffer(), functions.length(), sizeof(function_entry), sort_functions);
  for (size_t i = 0; i < functions.length(); i++) {
    function_entry &f = functions[i];
    symbol *sym = f.sym;

    assert(sym->addr() > 0);
    assert(sym->usage & uDEFINE);
    assert(sym->codeaddr > sym->addr());

    sp_file_publics_t &pubfunc = publics->add();
    pubfunc.address = sym->addr();
    pubfunc.name = names->add(pool, f.name.chars());

    sym->funcid = (uint32_t(i) << 1) | 1;
  }

  // Shuffle natives to be in address order.
  qsort(nativeList.buffer(), nativeList.length(), sizeof(symbol *), sort_by_addr);
  for (size_t i = 0; i < nativeList.length(); i++) {
    symbol *sym = nativeList[i];
    assert(size_t(sym->addr()) == i);

    sp_file_natives_t &entry = natives->add();

    char testalias[sNAMEMAX + 1];
    if (lookup_alias(testalias, sym->name))
      entry.name = names->add(pool, "@");
    else
      entry.name = names->add(pool, sym->name);
  }

  // Relocate all labels in the assembly buffer.
  relocate_labels(fin);

  // Generate buffers.
  Vector<cell> code_buffer, data_buffer;
  generate_segment(&code_buffer, fin, sIN_CSEG);
  generate_segment(&data_buffer, fin, sIN_DSEG);

  // Set up the code section.
  code->header().codesize = code_buffer.length() * sizeof(cell);
  code->header().cellsize = sizeof(cell);
  code->header().codeversion = SmxConsts::CODE_VERSION_JIT_1_1;
  code->header().flags = CODEFLAG_DEBUG;
  code->header().main = 0;
  code->header().code = sizeof(sp_file_code_t);
  code->setBlob((uint8_t *)code_buffer.buffer(), code_buffer.length() * sizeof(cell));

  // Set up the data section. Note pre-SourceMod 1.7, the |memsize| was
  // computed as AMX::stp, which included the entire memory size needed to
  // store the file. Here (in 1.7+), we allocate what is actually needed
  // by the plugin.
  data->header().datasize = data_buffer.length() * sizeof(cell);
  data->header().memsize =
    data->header().datasize +
    glb_declared * sizeof(cell) +
    pc_stksize * sizeof(cell);
  data->header().data = sizeof(sp_file_data_t);
  data->setBlob((uint8_t *)data_buffer.buffer(), data_buffer.length() * sizeof(cell));

  free(LabelTable);
  LabelTable = nullptr;

  // Add tables in the same order SourceMod 1.6 added them.
  builder.add(code);
  builder.add(data);
  builder.add(publics);
  builder.add(pubvars);
  builder.add(natives);
  builder.add(names);
  append_debug_tables(&builder, pool, names, nativeList);

  builder.write(buffer);
}

static void splat_to_binary(const char *binfname, void *bytes, size_t size)
{
  // Note: error 161 will setjmp(), which skips destructors :(
  FILE *fp = fopen(binfname, "wb");
  if (!fp) {
    error(FATAL_ERROR_WRITE, binfname);
    return;
  }
  if (fwrite(bytes, 1, size, fp) != size) {
    fclose(fp);
    error(FATAL_ERROR_WRITE, binfname);
    return;
  }
  fclose(fp);
}

void assemble(const char *binfname, void *fin)
{
  MemoryBuffer buffer;
  assemble_to_buffer(&buffer, fin);

  // Buffer compression logic. 
  sp_file_hdr_t *header = (sp_file_hdr_t *)buffer.bytes();
  size_t region_size = header->imagesize - header->dataoffs;
  size_t zbuf_max = compressBound(region_size);
  Bytef *zbuf = (Bytef *)malloc(zbuf_max);

  uLong new_disksize = zbuf_max;
  int err = compress2(
    zbuf, 
    &new_disksize,
    (Bytef *)(buffer.bytes() + header->dataoffs),
    region_size,
    Z_BEST_COMPRESSION
  );
  if (err != Z_OK) {
    free(zbuf);
    pc_printf("Unable to compress, error %d\n", err);
    pc_printf("Falling back to no compression.\n");
    splat_to_binary(binfname, buffer.bytes(), buffer.size());
    return;
  }

  header->disksize = new_disksize + header->dataoffs;
  header->compression = SmxConsts::FILE_COMPRESSION_GZ;

  buffer.rewind(header->dataoffs);
  buffer.write(zbuf, new_disksize);
  free(zbuf);

  splat_to_binary(binfname, buffer.bytes(), buffer.size());
}
