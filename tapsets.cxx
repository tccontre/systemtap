// tapset resolution
// Copyright (C) 2005 Red Hat Inc.
// Copyright (C) 2005 Intel Corporation.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "config.h"
#include "staptree.h"
#include "elaborate.h"
#include "tapsets.h"
#include "translate.h"

#include <deque>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <cstdarg>

extern "C" {
#include <fcntl.h>
#include <elfutils/libdwfl.h>
#include <elfutils/libdw.h>
#include <dwarf.h>
#include <elf.h>
#include <obstack.h>
#include "loc2c.h"

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
}

#include <fnmatch.h>

using namespace std;


// XXX: should standardize to these functions throughout translator

template <typename OUT, typename IN> inline OUT
lex_cast(IN const & in)
{
  stringstream ss;
  OUT out;
  if (!(ss << in && ss >> out))
    throw runtime_error("bad lexical cast");
  return out;
}

template <typename OUT, typename IN> inline OUT
lex_cast_hex(IN const & in)
{
  stringstream ss;
  OUT out;
  if (!(ss << hex << showbase << in && ss >> out))
    throw runtime_error("bad lexical cast");
  return out;
}


// return as quoted string, with at least '"' backslash-escaped
template <typename IN> inline string
lex_cast_qstring(IN const & in)
{
  stringstream ss;
  string out, out2;
  if (!(ss << in))
    throw runtime_error("bad lexical cast");
  out = ss.str();
  out2 += '"';
  for (unsigned i=0; i<out.length(); i++)
    {
      if (out[i] == '"') // XXX others?
	out2 += '\\';
      out2 += out[i];
    }
  out2 += '"';
  return out2;
}


// ------------------------------------------------------------------------
// begin/end probes are run right during registration / deregistration
// ------------------------------------------------------------------------

struct be_derived_probe: public derived_probe
{
  bool begin;
  be_derived_probe (probe* p, bool b): derived_probe (p), begin (b) {}
  be_derived_probe (probe* p, probe_point* l, bool b):
    derived_probe (p, l), begin (b) {}

  void emit_registrations (translator_output* o, unsigned i);
  void emit_deregistrations (translator_output* o, unsigned i);
  void emit_probe_entries (translator_output* o, unsigned i);
};


struct be_builder: public derived_probe_builder
{
  bool begin;
  be_builder(bool b) : begin(b) {}
  virtual void build(systemtap_session & sess,
		     probe * base,
		     probe_point * location,
		     std::map<std::string, literal *> const & parameters,
		     vector<derived_probe *> & finished_results)
  {
    finished_results.push_back(new be_derived_probe(base, location, begin));
  }
};


void
be_derived_probe::emit_registrations (translator_output* o, unsigned j)
{
  if (begin)
    for (unsigned i=0; i<locations.size(); i++)
      o->newline() << "enter_" << j << "_" << i << " ();";
}


void
be_derived_probe::emit_deregistrations (translator_output* o, unsigned j)
{
  if (!begin)
    for (unsigned i=0; i<locations.size(); i++)
      o->newline() << "enter_" << j << "_" << i << " ();";
}


void
be_derived_probe::emit_probe_entries (translator_output* o, unsigned j)
{
  for (unsigned i=0; i<locations.size(); i++)
    {
      probe_point *l = locations[i];
      o->newline() << "/* location " << i << ": " << *l << " */";
      o->newline() << "static void enter_" << j << "_" << i << " (void);";
      o->newline() << "void enter_" << j << "_" << i << " () {";

      // While begin/end probes are executed single-threaded, we
      // still code defensively and use a per-cpu context.
      o->newline(1) << "struct context* c = & contexts [smp_processor_id()];";
      o->newline() << "const char* probe_point = "
		   << lex_cast_qstring(*l) << ";";

      // A precondition for running a probe handler is that we're in STARTING
      // or STOPPING state (not ERROR), and that no one else is already using
      // this context.
      o->newline() << "if (atomic_read (&session_state) != ";
      if (begin) o->line() << "STAP_SESSION_STARTING)";
      else o->line() << "STAP_SESSION_STOPPING)";
      o->newline(1) << "return;";
      o->newline(-1) << "if (atomic_inc_return (&c->busy) != 1) {";
      o->newline(1) << "printk (KERN_ERR \"probe reentrancy (%s vs %s)\\n\", "
		    << "c->probe_point, probe_point);";
      o->newline() << "atomic_set (& session_state, STAP_SESSION_ERROR);";
      o->newline() << "atomic_dec (&c->busy);";
      o->newline() << "return;";
      o->newline(-1) << "}";
      o->newline();
      o->newline() << "c->last_error = 0;";
      o->newline() << "c->probe_point = probe_point;";
      o->newline() << "c->nesting = 0;";
      o->newline() << "c->regs = 0;";
      o->newline() << "c->actioncount = 0;";

      // NB: locals are initialized by probe function itself
      o->newline() << "probe_" << j << " (c);";

      o->newline() << "if (c->last_error && c->last_error[0]) {";
      o->newline(1) << "_stp_error (\"%s near %s\", c->last_error, c->last_stmt);";
      o->newline() << "atomic_set (& session_state, STAP_SESSION_ERROR);";
      o->newline(-1) << "}";

      o->newline() << "atomic_dec (&c->busy);";
      o->newline(-1) << "}" << endl;
    }
}


// ------------------------------------------------------------------------
//  Dwarf derived probes.
// ------------------------------------------------------------------------

static string TOK_PROCESS("process");
static string TOK_KERNEL("kernel");
static string TOK_MODULE("module");

static string TOK_FUNCTION("function");
static string TOK_INLINE("inline");
static string TOK_RETURN("return");
static string TOK_CALLEES("callees");

static string TOK_STATEMENT("statement");
static string TOK_LABEL("label");
static string TOK_RELATIVE("relative");




struct
func_info
{
  func_info()
    : decl_file(NULL), decl_line(-1), prologue_end(0)
  {
    memset(&die, 0, sizeof(die));
  }
  string name;
  char const * decl_file;
  int decl_line;
  Dwarf_Die die;
  Dwarf_Addr prologue_end;
};

struct
inline_instance_info
{
  inline_instance_info()
    : decl_file(NULL), decl_line(-1)
  {
    memset(&die, 0, sizeof(die));
  }
  string name;
  char const * decl_file;
  int decl_line;
  Dwarf_Die die;
};

static int
query_cu (Dwarf_Die * cudie, void * arg);


// Helper for dealing with selected portions of libdwfl in a more readable
// fashion, and with specific cleanup / checking / logging options.

static const char *
dwarf_diename_integrate (Dwarf_Die *die)
{
  Dwarf_Attribute attr_mem;
  return dwarf_formstring (dwarf_attr_integrate (die, DW_AT_name, &attr_mem));
}

struct
dwflpp
{
  systemtap_session & sess;
  Dwfl * dwfl;

  // These are "current" values we focus on.
  Dwfl_Module * module;
  Dwarf * module_dwarf;
  Dwarf_Addr module_bias;

  // These describe the current module's PC address range
  Dwarf_Addr module_start;
  Dwarf_Addr module_end;

  Dwarf_Die * cu;
  Dwarf_Die * function;

  string module_name;
  string cu_name;
  string function_name;


  string const default_name(char const * in,
			    char const * type)
  {
    if (in)
      return in;
    if (false && sess.verbose)
      clog << "WARNING: no name found for " << type << endl;
    return string("");
  }


  void get_module_dwarf(bool required = true)
  {
    if (!module_dwarf)
      module_dwarf = dwfl_module_getdwarf(module, &module_bias);

    if (!module_dwarf)
      {
	string msg = "cannot find ";
	if (module_name == "")
	  msg += "kernel";
	else
	  msg += string("module ") + module_name;
	msg += " debuginfo";

	int i = dwfl_errno();
	if (i)
	  msg += string(": ") + dwfl_errmsg (i);

	if (required)
	  throw semantic_error (msg);
	else
	  cerr << "WARNING: " << msg << endl;
      }
  }


  void focus_on_module(Dwfl_Module * m)
  {
    assert(m);
    module = m;
    module_name = default_name(dwfl_module_info(module, NULL,
						&module_start, &module_end,
						NULL, NULL,
						NULL, NULL),
			       "module");

    // Reset existing pointers and names

    module_dwarf = NULL;

    cu_name.clear();
    cu = NULL;

    function_name.clear();
    function = NULL;
  }


  void focus_on_cu(Dwarf_Die * c)
  {
    assert(c);
    assert(module);

    cu = c;
    cu_name = default_name(dwarf_diename(c), "CU");

    // Reset existing pointers and names
    function_name.clear();
    function = NULL;
  }


  void focus_on_function(Dwarf_Die * f)
  {
    assert(f);
    assert(module);
    assert(cu);

    function = f;
    function_name = default_name(dwarf_diename(function),
				 "function");
  }


  void focus_on_module_containing_global_address(Dwarf_Addr a)
  {
    assert(dwfl);
    cu = NULL;
    if (false && sess.verbose)
      clog << "focusing on module containing global addr " << a << endl;
    Dwfl_Module* mod = dwfl_addrmodule(dwfl, a);
    if (mod) // address could be wildly out of range
      focus_on_module(mod);
  }


  void query_cu_containing_global_address(Dwarf_Addr a, void *arg)
  {
    Dwarf_Addr bias;
    assert(dwfl);
    get_module_dwarf();
    Dwarf_Die* cudie = dwfl_module_addrdie(module, a, &bias);
    if (cudie) // address could be wildly out of range
      query_cu (cudie, arg);
    assert(bias == module_bias);
  }


  void query_cu_containing_module_address(Dwarf_Addr a, void *arg)
  {
    query_cu_containing_global_address(module_address_to_global(a), arg);
  }


  Dwarf_Addr module_address_to_global(Dwarf_Addr a)
  {
    assert(dwfl);
    assert(module);
    get_module_dwarf();
    if (module_name == TOK_KERNEL)
      return a;

    if (false && sess.verbose)
      clog << "module addr " << hex << a
	   << " + module start " << module_start
	   << " -> global addr " << (a + module_start) << dec << endl;
    return a + module_start;
  }


  Dwarf_Addr global_address_to_module(Dwarf_Addr a)
  {
    assert(module);
    get_module_dwarf();
    if (false && sess.verbose)
      clog << "global addr " << a
	   << " - module start " << hex << module_start
	   << " -> module addr " << (a - module_start) << dec << endl;
    return a - module_bias;
  }


  bool module_name_matches(string pattern)
  {
    assert(module);
    bool t = (fnmatch(pattern.c_str(), module_name.c_str(), 0) == 0);
    if (t && sess.verbose)
      clog << "pattern '" << pattern << "' "
	   << "matches "
	   << "module '" << module_name << "'" << endl;
    return t;
  }


  bool function_name_matches(string pattern)
  {
    assert(function);
    bool t = (fnmatch(pattern.c_str(), function_name.c_str(), 0) == 0);
    if (t && sess.verbose)
      clog << "pattern '" << pattern << "' "
	   << "matches "
	   << "function '" << function_name << "'" << endl;
    return t;
  }


  bool cu_name_matches(string pattern)
  {
    assert(cu);
    bool t = (fnmatch(pattern.c_str(), cu_name.c_str(), 0) == 0);
    if (t && sess.verbose)
      clog << "pattern '" << pattern << "' "
	   << "matches "
	   << "CU '" << cu_name << "'" << endl;
    return t;
  }


  void dwfl_assert(string desc, int rc) // NB: "rc == 0" means OK in this case
  {
    string msg = "libdwfl failure (" + desc + "): ";
    if (rc < 0) msg += dwfl_errmsg (rc);
    else if (rc > 0) msg += strerror (rc);
    if (rc != 0)
      throw semantic_error (msg);
  }

  void dwarf_assert(string desc, int rc) // NB: "rc == 0" means OK in this case
  {
    string msg = "libdw failure (" + desc + "): ";
    if (rc < 0) msg += dwarf_errmsg (rc);
    else if (rc > 0) msg += strerror (rc);
    if (rc != 0)
      throw semantic_error (msg);
  }


  dwflpp(systemtap_session & sess)
    :
    sess(sess),
    dwfl(NULL),
    module(NULL),
    module_dwarf(NULL),
    module_bias(0),
    module_start(0),
    module_end(0),
    cu(NULL),
    function(NULL)
  {}


  void setup(bool kernel)
  {
    // XXX: this is where the session -R parameter could come in
    static char* debuginfo_path = "-:.debug:/usr/lib/debug";

    static const Dwfl_Callbacks proc_callbacks =
      {
	dwfl_linux_proc_find_elf,
	dwfl_standard_find_debuginfo,
	NULL,
        & debuginfo_path
      };

    static const Dwfl_Callbacks kernel_callbacks =
      {
	dwfl_linux_kernel_find_elf,
	dwfl_standard_find_debuginfo,
	dwfl_linux_kernel_module_section_address,
        & debuginfo_path
      };

    if (kernel)
      {
	dwfl = dwfl_begin (&kernel_callbacks);
	if (!dwfl)
	  throw semantic_error ("cannot open dwfl");
	dwfl_report_begin (dwfl);
        // XXX: if we have only kernel.* probe points, we shouldn't waste time
        // looking for module debug-info (and vice versa).
	dwfl_assert ("dwfl_linux_kernel_report_kernel",
		     dwfl_linux_kernel_report_kernel (dwfl));
	dwfl_assert ("dwfl_linux_kernel_report_modules",
		     dwfl_linux_kernel_report_modules (dwfl));
	// NB: While RH bug #169672 prevents detection of -debuginfo absence
	// here, the get_module_dwarf() function will throw an exception
	// before long.
      }
    else
      {
	dwfl = dwfl_begin (&proc_callbacks);
	dwfl_report_begin (dwfl);
	if (!dwfl)
	  throw semantic_error ("cannot open dwfl");
	// XXX: Find pids or processes, do userspace stuff.
      }

    dwfl_assert ("dwfl_report_end", dwfl_report_end(dwfl, NULL, NULL));
  }

  void iterate_over_modules(int (* callback)(Dwfl_Module *, void **,
					     const char *, Dwarf_Addr,
					     void *),
			    void * data)
  {
    ptrdiff_t off = 0;
    do
      {
	off = dwfl_getmodules (dwfl, callback, data, off);
      }
    while (off > 0);
    dwfl_assert("dwfl_getmodules", off);
  }


  void iterate_over_cus (int (*callback)(Dwarf_Die * die, void * arg),
			 void * data)
  {
    get_module_dwarf(false);

    if (!module_dwarf)
      return;

    Dwarf *dw = module_dwarf;
    Dwarf_Off off = 0;
    size_t cuhl;
    Dwarf_Off noff;
    while (dwarf_nextcu (dw, off, &noff, &cuhl, NULL, NULL, NULL) == 0)
      {
	Dwarf_Die die_mem;
	Dwarf_Die *die;
	die = dwarf_offdie (dw, off + cuhl, &die_mem);
	if (callback (die, data) != DWARF_CB_OK)
	  break;
	off = noff;
      }
  }


  bool func_is_inline()
  {
    assert (function);
    return dwarf_func_inline (function) != 0;
  }

  void iterate_over_inline_instances (int (* callback)(Dwarf_Die * die, void * arg),
				      void * data)
  {
    assert (function);
    assert (func_is_inline ());
    dwarf_assert ("dwarf_func_inline_instances",
		  dwarf_func_inline_instances (function, callback, data));
  }


  void iterate_over_functions (int (* callback)(Dwarf_Die * func, void * arg),
			       void * data)
  {
    assert (module);
    assert (cu);
    dwarf_getfuncs (cu, callback, data, 0);
  }


  bool has_single_line_record (char const * srcfile, int lineno)
  {
    if (lineno < 0)
      return false;

    Dwarf_Line **srcsp = NULL;
    size_t nsrcs = 0;

    dwarf_assert ("dwarf_getsrc_file",
		  dwarf_getsrc_file (module_dwarf,
				     srcfile, lineno, 0,
				     &srcsp, &nsrcs));

    return nsrcs == 1;
  }

  void iterate_over_srcfile_lines (char const * srcfile,
				   int lineno,
				   bool need_single_match,
				   void (* callback) (Dwarf_Line * line, void * arg),
				   void *data)
  {
    Dwarf_Line **srcsp = NULL;
    size_t nsrcs = 0;

    get_module_dwarf();

    dwarf_assert ("dwarf_getsrc_file",
		  dwarf_getsrc_file (module_dwarf,
				     srcfile, lineno, 0,
				     &srcsp, &nsrcs));

    if (need_single_match && nsrcs > 1)
      {
	// We wanted a single line record (a unique address for the
	// line) and we got a bunch of line records. We're going to
	// skip this probe (throw an exception) but before we throw
	// we're going to look around a bit to see if there's a low or
	// high line number nearby which *doesn't* have this problem,
	// so we can give the user some advice.

	int lo_try = -1;
	int hi_try = -1;
	for (size_t i = 1; i < 6; ++i)
	  {
	    if (lo_try == -1 && has_single_line_record(srcfile, lineno - i))
	      lo_try = lineno - i;

	    if (hi_try == -1 && has_single_line_record(srcfile, lineno + i))
	      hi_try = lineno + i;
	  }

	string advice = "";
	if (lo_try > 0 || hi_try > 0)
	  advice = " (try "
	    + (lo_try > 0
	       ? (string(srcfile) + ":" + lex_cast<string>(lo_try))
	       : string(""))
	    + (lo_try > 0 && hi_try > 0 ? " or " : "")
	    + (hi_try > 0
	       ? (string(srcfile) + ":"+ lex_cast<string>(hi_try))
	       : string(""))
	    + ")";

	throw semantic_error("multiple addresses for "
			     + string(srcfile)
			     + ":"
			     + lex_cast<string>(lineno)
			     + advice);
      }

    try
      {
	for (size_t i = 0; i < nsrcs; ++i)
	  {
	    callback (srcsp[i], data);
	  }
      }
    catch (...)
      {
	free (srcsp);
	throw;
      }
    free (srcsp);
  }


  void collect_srcfiles_matching (string const & pattern,
				  set<char const *> & filtered_srcfiles)
  {
    assert (module);
    assert (cu);

    size_t nfiles;
    Dwarf_Files *srcfiles;

    dwarf_assert ("dwarf_getsrcfiles",
		  dwarf_getsrcfiles (cu, &srcfiles, &nfiles));
    {
    for (size_t i = 0; i < nfiles; ++i)
      {
	char const * fname = dwarf_filesrc (srcfiles, i, NULL, NULL);
	if (fnmatch (pattern.c_str(), fname, 0) == 0)
	  {
	    filtered_srcfiles.insert (fname);
	    if (sess.verbose)
	      clog << "selected source file '" << fname << "'" << endl;
	  }
      }
    }
  }

  void resolve_prologue_endings (map<Dwarf_Addr, func_info> & funcs)
  {
    assert(module);
    assert(cu);

    size_t nlines;
    Dwarf_Lines *lines;
    Dwarf_Addr previous_addr;
    bool choose_next_line = false;

    dwarf_assert ("dwarf_getsrclines",
		  dwarf_getsrclines(cu, &lines, &nlines));

    for (size_t i = 0; i < nlines; ++i)
      {
	Dwarf_Addr addr;
	Dwarf_Line * line_rec = dwarf_onesrcline(lines, i);
	dwarf_lineaddr (line_rec, &addr);

	if (choose_next_line)
	  {
	    map<Dwarf_Addr, func_info>::iterator i = funcs.find (previous_addr);
	    assert (i != funcs.end());
	    i->second.prologue_end = addr;
	    choose_next_line = false;
	  }

	map<Dwarf_Addr, func_info>::const_iterator i = funcs.find (addr);
	if (i != funcs.end())
	  choose_next_line = true;
	previous_addr = addr;
      }

    // XXX: free lines[] ?
  }


  void resolve_prologue_endings2 (map<Dwarf_Addr, func_info> & funcs)
  {
    // This heuristic attempts to pick the first address that has a
    // source line distinct from the function declaration's (entrypc's).
    // This should be the first statement *past* the prologue.
    assert(module);
    assert(cu);

    size_t nlines;
    Dwarf_Lines *lines;
    Dwarf_Addr last_function_entrypc;
    int choose_next_line_otherthan = -1;

    // XXX: ideally, there would be a dwarf_getfile(line) routine,
    // so that we compare not just a line number mismatch, but a
    // file name mismatch too.
    //
    // If the first statement of a function is into some inline
    // function, we'll be scanning over Dwarf_Line objects that have,
    // chances are, wildly different lineno's.  If luck turns against
    // us, and that inline function body happens to be defined in a
    // different file but at the same line number as its caller, then
    // we will get slightly messed up.

    dwarf_assert ("dwarf_getsrclines",
		  dwarf_getsrclines(cu, &lines, &nlines));

    for (size_t i = 0; i < nlines; ++i)
      {
	Dwarf_Addr addr;
	Dwarf_Line * line_rec = dwarf_onesrcline(lines, i);
	dwarf_lineaddr (line_rec, &addr);
        int this_lineno;

        dwfl_assert ("dwarf_lineno",
                     dwarf_lineno(line_rec, &this_lineno));

	if (choose_next_line_otherthan >= 0 &&
            this_lineno != choose_next_line_otherthan)
	  {
	    map<Dwarf_Addr, func_info>::iterator i =
              funcs.find (last_function_entrypc);
	    assert (i != funcs.end());
            Dwarf_Addr addr0 = i->second.prologue_end;
            if (addr0 != addr)
              {
                i->second.prologue_end = addr;
		if (sess.verbose)
		  clog << "prologue disagreement: " << i->second.name
		       << " heur0=" << hex << addr0
		       << " heur1=" << addr << dec
		       << endl;
              }
	    choose_next_line_otherthan = -1;
	  }

	map<Dwarf_Addr, func_info>::const_iterator i = funcs.find (addr);
	if (i != funcs.end())
          {
            dwfl_assert ("dwarf_lineno",
                         dwarf_lineno(line_rec, &choose_next_line_otherthan));
            last_function_entrypc = addr;
          }
      }

    // XXX: free lines[] ?
  }


  bool function_entrypc (Dwarf_Addr * addr)
  {
    assert (function);
    return (dwarf_entrypc (function, addr) == 0);
  }


  bool die_entrypc (Dwarf_Die * die, Dwarf_Addr * addr)
  {
    Dwarf_Attribute attr_mem;
    Dwarf_Attribute *attr = dwarf_attr (die, DW_AT_entry_pc, &attr_mem);
    if (attr != NULL)
      return (dwarf_formaddr (attr, addr) == 0);

    return ( dwarf_lowpc (die, addr) == 0);
  }

  void function_die (Dwarf_Die *d)
  {
    assert (function);
    *d = *function;
  }

  void function_file (char const ** c)
  {
    assert (function);
    assert (c);
    *c = dwarf_decl_file (function);
  }

  void function_line (int *linep)
  {
    assert (function);
    dwarf_decl_line (function, linep);
  }

  bool die_has_pc (Dwarf_Die * die, Dwarf_Addr pc)
  {
    int res = dwarf_haspc (die, pc);
    if (res == -1)
      dwarf_assert ("dwarf_haspc", res);
    return res == 1;
  }


  static void loc2c_error (void *arg, const char *fmt, ...)
  {
    char *msg = NULL;
    va_list ap;
    va_start (ap, fmt);
    vasprintf (&msg, fmt, ap);
    va_end (ap);
    throw semantic_error (msg);
  }


  static void loc2c_emit_address (void *arg, struct obstack *pool,
				  Dwarf_Addr address)
  {
    dwflpp *dwfl = (dwflpp *) arg;
    obstack_printf (pool, "%#" PRIx64 "UL /* hard-coded %s address */",
		    address, dwfl_module_info (dwfl->module, NULL, NULL, NULL,
					       NULL, NULL, NULL, NULL));
  }

  Dwarf_Attribute *
  find_variable_and_frame_base (Dwarf_Die *scope_die,
				Dwarf_Addr pc,
				string const & local,
				Dwarf_Die *vardie,
				Dwarf_Attribute *fb_attr_mem)
  {
    Dwarf_Die *scopes;
    int nscopes = 0;
    Dwarf_Attribute *fb_attr = NULL;

    assert (cu);

    if (scope_die)
      nscopes = dwarf_getscopes_die (scope_die, &scopes);
    else
      nscopes = dwarf_getscopes (cu, pc, &scopes);

    if (nscopes == 0)
      {
	throw semantic_error ("unable to find any scopes containing "
			      + lex_cast_hex<string>(pc)
			      + " while searching for local '" + local + "'");
      }

    int declaring_scope = dwarf_getscopevar (scopes, nscopes,
					     local.c_str(),
					     0, NULL, 0, 0,
					     vardie);
    if (declaring_scope < 0)
      {
	throw semantic_error ("unable to find local '" + local + "'"
			      + " near pc " + lex_cast_hex<string>(pc));
      }

    for (int inner = 0; inner < nscopes; ++inner)
      {
	switch (dwarf_tag (&scopes[inner]))
	  {
	  default:
	    continue;
	  case DW_TAG_subprogram:
	  case DW_TAG_entry_point:
	  case DW_TAG_inlined_subroutine:  /* XXX */
	    if (inner >= declaring_scope)
	      fb_attr = dwarf_attr_integrate (&scopes[inner],
					      DW_AT_frame_base,
					      fb_attr_mem);
	    break;
	  }
      }
    return fb_attr;
  }


  Dwarf_Die *
  translate_components(struct obstack *pool,
		       struct location **tail,
		       Dwarf_Addr pc,
		       vector<pair<target_symbol::component_type,
		       std::string> > const & components,
		       Dwarf_Die *vardie,
		       Dwarf_Die *die_mem,
		       Dwarf_Attribute *attr_mem)
  {
    Dwarf_Die *die = vardie;
    unsigned i = 0;
    while (i < components.size())
      {
	die = dwarf_formref_die (attr_mem, die_mem);
	const int typetag = dwarf_tag (die);
	switch (typetag)
	  {
	  case DW_TAG_typedef:
	  case DW_TAG_const_type:
	  case DW_TAG_volatile_type:
	    /* Just iterate on the referent type.  */
	    break;

	  case DW_TAG_pointer_type:
	    if (components[i].first == target_symbol::comp_literal_array_index)
	      goto subscript;

	    c_translate_pointer (pool, 1, module_bias, die, tail);
	    break;

	  case DW_TAG_array_type:
	    if (components[i].first == target_symbol::comp_literal_array_index)
	      {
	      subscript:
		c_translate_array (pool, 1, module_bias, die, tail,
				   NULL, lex_cast<Dwarf_Word>(components[i].second));
		++i;
	      }
	    else
	      throw semantic_error("bad field '"
				   + components[i].second
				   + "' for array type");
	    break;

	  case DW_TAG_structure_type:
	  case DW_TAG_union_type:
	    switch (dwarf_child (die, die_mem))
	      {
	      case 1:		/* No children.  */
		throw semantic_error ("empty struct "
				      + string (dwarf_diename_integrate (die) ?: "<anonymous>"));
		break;
	      case -1:		/* Error.  */
	      default:		/* Shouldn't happen */
		throw semantic_error (string (typetag == DW_TAG_union_type ? "union" : "struct")
				      + string (dwarf_diename_integrate (die) ?: "<anonymous>")
				      + string (dwarf_errmsg (-1)));
		break;

	      case 0:
		break;
	      }

	    while (dwarf_tag (die) != DW_TAG_member
		   || ({ const char *member = dwarf_diename_integrate (die);
		       member == NULL || string(member) != components[i].second; }))
	      if (dwarf_siblingof (die, die_mem) != 0)
		throw semantic_error ("field name " + components[i].second + " not found");

	    if (dwarf_attr_integrate (die, DW_AT_data_member_location,
				      attr_mem) == NULL)
	      {
		/* Union members don't usually have a location,
		   but just use the containing union's location.  */
		if (typetag != DW_TAG_union_type)
		  throw semantic_error ("no location for field "
					+ components[i].second
					+ " :" + string(dwarf_errmsg (-1)));
	      }
	    else
	      c_translate_location (pool, NULL, NULL, NULL, 1,
				    module_bias, attr_mem, pc,
				    tail, NULL);
	    ++i;
	    break;

	  case DW_TAG_base_type:
	    throw semantic_error ("field "
				  + components[i].second
				  + " vs base type "
				  + string(dwarf_diename_integrate (die) ?: "<anonymous type>"));
	    break;
	  case -1:
	    throw semantic_error ("cannot find type: " + string(dwarf_errmsg (-1)));
	    break;

	  default:
	    throw semantic_error (string(dwarf_diename_integrate (die) ?: "<anonymous type>")
				  + ": unexpected type tag "
				  + lex_cast<string>(dwarf_tag (die)));
	    break;
	  }

	/* Now iterate on the type in DIE's attribute.  */
	if (dwarf_attr_integrate (die, DW_AT_type, attr_mem) == NULL)
	  throw semantic_error ("cannot get type of field: " + string(dwarf_errmsg (-1)));
      }
    return die;
  }


  Dwarf_Die *
  resolve_unqualified_inner_typedie (Dwarf_Die *typedie_mem,
				     Dwarf_Attribute *attr_mem)
  {
    ;
    Dwarf_Die *typedie;
    int typetag = 0;
    while (1)
      {
	typedie = dwarf_formref_die (attr_mem, typedie_mem);
	if (typedie == NULL)
	  throw semantic_error ("cannot get type: " + string(dwarf_errmsg (-1)));
	typetag = dwarf_tag (typedie);
	if (typetag != DW_TAG_typedef &&
	    typetag != DW_TAG_const_type &&
	    typetag != DW_TAG_volatile_type)
	  break;
	if (dwarf_attr_integrate (typedie, DW_AT_type, attr_mem) == NULL)
	  throw semantic_error ("cannot get type of pointee: " + string(dwarf_errmsg (-1)));
      }
    return typedie;
  }


  void
  translate_final_fetch_or_store (struct obstack *pool,
				  struct location **tail,
				  Dwarf_Addr module_bias,
				  Dwarf_Die *die,
				  Dwarf_Attribute *attr_mem,
				  bool lvalue,
				  string & prelude,
				  string & postlude,
				  exp_type & ty)
  {
    /* First boil away any qualifiers associated with the type DIE of
       the final location to be accessed.  */

    Dwarf_Die typedie_mem;
    Dwarf_Die *typedie;
    int typetag;

    typedie = resolve_unqualified_inner_typedie (&typedie_mem, attr_mem);
    typetag = dwarf_tag (typedie);

    /* Then switch behavior depending on the type of fetch/store we
       want, and the type and pointer-ness of the final location. */

    switch (typetag)
      {
      default:
	throw semantic_error ("unsupported type tag "
			      + lex_cast<string>(typetag));
	break;

      case DW_TAG_enumeration_type:
      case DW_TAG_base_type:
	ty = pe_long;
	if (lvalue)
	  c_translate_store (pool, 1, module_bias, die, typedie, tail,
			     "THIS->value");
	else
	  c_translate_fetch (pool, 1, module_bias, die, typedie, tail,
			     "THIS->__retvalue");
	break;

      case DW_TAG_array_type:
      case DW_TAG_pointer_type:

	if (lvalue)
	  throw semantic_error ("cannot store into target pointer value");

	{
	  Dwarf_Die pointee_typedie_mem;
	  Dwarf_Die *pointee_typedie;
	  Dwarf_Word pointee_encoding;
	  Dwarf_Word pointee_byte_size = 0;

	  if (dwarf_attr_integrate (typedie, DW_AT_type, attr_mem) == NULL)
	    throw semantic_error ("cannot get type of pointer: " + string(dwarf_errmsg (-1)));

	  pointee_typedie = resolve_unqualified_inner_typedie (&pointee_typedie_mem, attr_mem);

	  if (dwarf_attr_integrate (pointee_typedie, DW_AT_byte_size, attr_mem))
	    dwarf_formudata (attr_mem, &pointee_byte_size);

	  dwarf_formudata (dwarf_attr_integrate (pointee_typedie, DW_AT_encoding, attr_mem),
			   &pointee_encoding);

	  // We have the pointer: cast it to an integral type via &(*(...))

	  // NB: per bug #1187, at one point char*-like types were
	  // automagically converted here to systemtap string values.
	  // For several reasons, this was taken back out, leaving
	  // pointer-to-string "conversion" (copying) to tapset functions.

	  ty = pe_long;
	  if (typetag == DW_TAG_array_type)
	    c_translate_array (pool, 1, module_bias, typedie, tail, NULL, 0);
	  else
	    c_translate_pointer (pool, 1, module_bias, typedie, tail);
	  c_translate_addressof (pool, 1, module_bias, NULL, pointee_typedie, tail,
				 "THIS->__retvalue");
	}
	break;
      }
  }


  string
  literal_stmt_for_local (Dwarf_Die *scope_die,
			  Dwarf_Addr pc,
			  string const & local,
			  vector<pair<target_symbol::component_type,
			  std::string> > const & components,
			  bool lvalue,
			  exp_type & ty)
  {
    Dwarf_Die vardie;
    Dwarf_Attribute fb_attr_mem, *fb_attr = NULL;

    fb_attr = find_variable_and_frame_base (scope_die, pc, local,
					    &vardie, &fb_attr_mem);

    if (sess.verbose)
      clog << "finding location for local '" << local
	   << "' near address " << hex << pc
	   << ", module bias " << module_bias << dec
	   << endl;

    Dwarf_Attribute attr_mem;
    if (dwarf_attr_integrate (&vardie, DW_AT_location, &attr_mem) == NULL)
      {
	throw semantic_error("failed to retrieve location "
			     "attribute for local '" + local
			     + "' (dieoffset: "
			     + lex_cast_hex<string>(dwarf_dieoffset (&vardie))
			     + ")");
      }

#define obstack_chunk_alloc malloc
#define obstack_chunk_free free

    struct obstack pool;
    obstack_init (&pool);
    struct location *tail = NULL;

    /* Given $foo->bar->baz[NN], translate the location of foo. */

    struct location *head = c_translate_location (&pool, &loc2c_error, this,
						  &loc2c_emit_address,
						  1, module_bias,
						  &attr_mem, pc,
						  &tail, fb_attr);

    if (dwarf_attr_integrate (&vardie, DW_AT_type, &attr_mem) == NULL)
      throw semantic_error("failed to retrieve type "
			   "attribute for local '" + local + "'");


    /* Translate the ->bar->baz[NN] parts. */

    Dwarf_Die die_mem, *die = NULL;
    die = translate_components (&pool, &tail, pc, components,
				&vardie, &die_mem, &attr_mem);

    /* Translate the assignment part, either
       x = $foo->bar->baz[NN]
       or
       $foo->bar->baz[NN] = x
    */

    string prelude, postlude;
    translate_final_fetch_or_store (&pool, &tail, module_bias,
				    die, &attr_mem, lvalue,
				    prelude, postlude, ty);

    /* Write the translation to a string. */

    size_t bufsz = 1024;
    char *buf = static_cast<char*>(malloc(bufsz));
    assert(buf);

    FILE *memstream = open_memstream (&buf, &bufsz);
    assert(memstream);

    fprintf(memstream, "{\n");
    fprintf(memstream, prelude.c_str());
    bool deref = c_emit_location (memstream, head, 1);
    fprintf(memstream, postlude.c_str());
    fprintf(memstream, "  goto out;\n");

    // dummy use of deref_fault label, to disable warning if deref() not used
    fprintf(memstream, "if (0) goto deref_fault;\n");

    // XXX: deref flag not reliable; emit fault label unconditionally
    // XXX: print the faulting address, like the user_string/kernel_string
    // tapset functions do
    if (deref) ;
    fprintf(memstream,
            "deref_fault:\n"
            "  c->last_error = \"pointer dereference fault\";\n"
            "  goto out;\n");
    fprintf(memstream, "}\n");

    fclose (memstream);
    string result(buf);
    free (buf);
    return result;
  }



  ~dwflpp()
  {
    if (dwfl)
      dwfl_end(dwfl);
  }
};


enum
function_spec_type
  {
    function_alone,
    function_and_file,
    function_file_and_line
  };


struct dwarf_builder;
struct dwarf_query;


struct dwarf_derived_probe : public derived_probe
{
  dwarf_derived_probe (Dwarf_Die *scope_die,
		       Dwarf_Addr addr,
		       dwarf_query & q);

  vector<Dwarf_Addr> probe_points;
  bool has_return;

  void add_probe_point(string const & funcname,
		       char const * filename,
		       int line,
		       Dwarf_Addr addr,
		       dwarf_query & q);

  // Pattern registration helpers.
  static void register_relative_variants(match_node * root,
					 dwarf_builder * dw);
  static void register_statement_variants(match_node * root,
					  dwarf_builder * dw);
  static void register_function_variants(match_node * root,
					  dwarf_builder * dw);
  static void register_inline_variants(match_node * root,
				       dwarf_builder * dw);
  static void register_function_and_statement_variants(match_node * root,
						       dwarf_builder * dw);
  static void register_patterns(match_node * root);

  virtual void emit_registrations (translator_output * o, unsigned i);
  virtual void emit_deregistrations (translator_output * o, unsigned i);
  virtual void emit_probe_entries (translator_output * o, unsigned i);
};

// Helper struct to thread through the dwfl callbacks.
struct
dwarf_query
{
  dwarf_query(systemtap_session & sess,
	      probe * base_probe,
	      probe_point * base_loc,
	      dwflpp & dw,
	      map<string, literal *> const & params,
	      vector<derived_probe *> & results);

  systemtap_session & sess;

  // Parameter extractors.
  static bool has_null_param(map<string, literal *> const & params,
			     string const & k);
  static bool get_string_param(map<string, literal *> const & params,
			       string const & k, string & v);
  static bool get_number_param(map<string, literal *> const & params,
			       string const & k, long & v);
  static bool get_number_param(map<string, literal *> const & params,
			       string const & k, Dwarf_Addr & v);

  string pt_regs_member_for_regnum(uint8_t dwarf_regnum);

  // Result vector and flavour-sorting mechanism.
  vector<derived_probe *> & results;
  bool probe_has_no_target_variables;
  map<string, dwarf_derived_probe *> probe_flavours;
  void add_probe_point(string const & funcname,
		       char const * filename,
		       int line,
		       Dwarf_Die *scope_die,
		       Dwarf_Addr addr);

  // Extracted parameters.
  bool has_kernel;
  bool has_process;
  bool has_module;
  string process_val;
  string module_val;
  string function_val;

  bool has_inline_str;
  bool has_function_str;
  bool has_statement_str;
  bool has_inline_num;
  bool has_function_num;
  bool has_statement_num;
  string statement_str_val;
  string function_str_val;
  string inline_str_val;
  Dwarf_Addr statement_num_val;
  Dwarf_Addr function_num_val;
  Dwarf_Addr inline_num_val;

  bool has_callees;
  long callee_val;

  bool has_return;

  bool has_label;
  string label_val;

  bool has_relative;
  long relative_val;

  function_spec_type parse_function_spec(string & spec);
  function_spec_type spec_type;
  string function;
  string file;
  int line;

  set<char const *> filtered_srcfiles;

  // Map official entrypc -> func_info object
  map<Dwarf_Addr, inline_instance_info> filtered_inlines;
  map<Dwarf_Addr, func_info> filtered_functions;
  bool choose_next_line;
  Dwarf_Addr entrypc_for_next_line;

  probe * base_probe;
  probe_point * base_loc;
  dwflpp & dw;
};


struct dwarf_builder: public derived_probe_builder
{
  dwarf_builder() {}
  virtual void build(systemtap_session & sess,
		     probe * base,
		     probe_point * location,
		     std::map<std::string, literal *> const & parameters,
		     vector<derived_probe *> & finished_results);
};

bool
dwarf_query::has_null_param(map<string, literal *> const & params,
			    string const & k)
{
  map<string, literal *>::const_iterator i = params.find(k);
  if (i != params.end() && i->second == NULL)
    return true;
  return false;
}

bool
dwarf_query::get_string_param(map<string, literal *> const & params,
			      string const & k, string & v)
{
  return derived_probe_builder::get_param (params, k, v);
}

bool
dwarf_query::get_number_param(map<string, literal *> const & params,
			      string const & k, long & v)
{
  int64_t value;
  bool present = derived_probe_builder::get_param (params, k, value);
  v = (long) value;
  return present;
}

bool
dwarf_query::get_number_param(map<string, literal *> const & params,
			      string const & k, Dwarf_Addr & v)
{
  int64_t value;
  bool present = derived_probe_builder::get_param (params, k, value);
  v = (Dwarf_Addr) value;
  return present;
}


dwarf_query::dwarf_query(systemtap_session & sess,
			 probe * base_probe,
			 probe_point * base_loc,
			 dwflpp & dw,
			 map<string, literal *> const & params,
			 vector<derived_probe *> & results)
  : sess(sess),
    results(results),
    probe_has_no_target_variables(false),
    base_probe(base_probe),
    base_loc(base_loc),
    dw(dw)
{

  // Reduce the query to more reasonable semantic values (booleans,
  // extracted strings, numbers, etc).

  has_kernel = has_null_param(params, TOK_KERNEL);
  has_module = get_string_param(params, TOK_MODULE, module_val);
  has_process = get_string_param(params, TOK_PROCESS, process_val);

  has_function_str = get_string_param(params, TOK_FUNCTION, function_str_val);
  has_function_num = get_number_param(params, TOK_FUNCTION, function_num_val);

  has_inline_str = get_string_param(params, TOK_INLINE, inline_str_val);
  has_inline_num = get_number_param(params, TOK_INLINE, inline_num_val);

  has_statement_str = get_string_param(params, TOK_STATEMENT, statement_str_val);
  has_statement_num = get_number_param(params, TOK_STATEMENT, statement_num_val);

  callee_val = 1;
  has_callees = (has_null_param(params, TOK_CALLEES) ||
		 get_number_param(params, TOK_CALLEES, callee_val));

  has_return = has_null_param(params, TOK_RETURN);

  has_label = get_string_param(params, TOK_LABEL, label_val);
  has_relative = get_number_param(params, TOK_RELATIVE, relative_val);

  if (has_function_str)
    spec_type = parse_function_spec(function_str_val);
  else if (has_inline_str)
    spec_type = parse_function_spec(inline_str_val);
  else if (has_statement_str)
    spec_type = parse_function_spec(statement_str_val);
}


function_spec_type
dwarf_query::parse_function_spec(string & spec)
{
  string::const_iterator i = spec.begin(), e = spec.end();

  function.clear();
  file.clear();
  line = 0;

  while (i != e && *i != '@')
    {
      if (*i == ':')
	goto bad;
      function += *i++;
    }

  if (i == e)
    {
      if (sess.verbose)
	clog << "parsed '" << spec
	     << "' -> func '" << function
	     << "'" << endl;
      return function_alone;
    }

  if (i++ == e)
    goto bad;

  while (i != e && *i != ':')
    file += *i++;

  if (i == e)
    {
      if (sess.verbose)
	clog << "parsed '" << spec
	     << "' -> func '"<< function
	     << "', file '" << file
	     << "'" << endl;
      return function_and_file;
    }

  if (i++ == e)
    goto bad;

  try
    {
      line = lex_cast<int>(string(i, e));
      if (sess.verbose)
	clog << "parsed '" << spec
	     << "' -> func '"<< function
	     << "', file '" << file
	     << "', line " << line << endl;
      return function_file_and_line;
    }
  catch (runtime_error & exn)
    {
      goto bad;
    }

 bad:
    throw semantic_error("malformed specification '" + spec + "'",
			 base_probe->tok);
}



// Our goal here is to calculate a "flavour", a string which
// characterizes the way in which this probe body depends on target
// variables. The flavour is used to separate instances of a dwarf
// probe which have different contextual bindings for the target
// variables which occur within the probe body. If two die/addr
// combinations have the same flavour string, they will be directed
// into the same probe function.

struct
target_variable_flavour_calculating_visitor
  : public traversing_visitor
{
  string flavour;

  dwarf_query & q;
  Dwarf_Die *scope_die;
  Dwarf_Addr addr;

  target_variable_flavour_calculating_visitor(dwarf_query & q,
					      Dwarf_Die *sd,
					      Dwarf_Addr a)
    : q(q), scope_die(sd), addr(a)
  {}
  void visit_target_symbol (target_symbol* e);
};

void
target_variable_flavour_calculating_visitor::visit_target_symbol (target_symbol *e)
{
  assert(e->base_name.size() > 0 && e->base_name[0] == '$');

  try
    {
      bool lvalue = is_active_lvalue(e);
      if (lvalue && !q.sess.guru_mode)
	throw semantic_error("Writing to target variable outside of guru mode", e->tok);

      flavour += lvalue ? 'w' : 'r';
      exp_type ty;
      string expr = q.dw.literal_stmt_for_local(scope_die,
						addr,
						e->base_name.substr(1),
						e->components,
						lvalue,
						ty);
      switch (ty)
	{
	case pe_unknown:
	  flavour += 'U';
	  break;
	case pe_long:
	  flavour += 'L';
	  break;
	case pe_string:
	  flavour += 'S';
	  break;
	case pe_stats:
	  flavour += 'T';
	  break;
	}
      flavour += lex_cast<string>(expr.size());
      flavour += '{';
      flavour += expr;
      flavour += '}';
    }
  catch (const semantic_error& er)
    {
      semantic_error er2 (er);
      er2.tok1 = e->tok;
      q.sess.print_error (er2);
    }
}


void
dwarf_query::add_probe_point(string const & funcname,
			     char const * filename,
			     int line,
			     Dwarf_Die *scope_die,
			     Dwarf_Addr addr)
{
  dwarf_derived_probe *probe = NULL;

  // Check whether the given address points into an .init section,
  // which will have been unmapped by the kernel by the time we get to
  // insert the probe.  In this case, just ignore this call.
  Dwarf_Addr baseaddr;
  Elf* elf = dwfl_module_getelf (dw.module, & baseaddr);
  Dwarf_Addr rel_addr = addr - baseaddr;
  if (elf)
    {
      // Iterate through section headers to find which one
      // contains the given rel_addr.
      Elf_Scn* scn = 0;
      size_t shstrndx;
      dw.dwfl_assert ("getshstrndx", elf_getshstrndx (elf, &shstrndx));
      while ((scn = elf_nextscn (elf, scn)) != NULL)
        {
          GElf_Shdr shdr_mem;
          GElf_Shdr *shdr = gelf_getshdr (scn, &shdr_mem);
          if (! shdr) continue; // XXX error?

          // check for address inclusion
          GElf_Addr start = shdr->sh_addr;
          GElf_Addr end = start + shdr->sh_size;
          if (! (rel_addr >= start && rel_addr < end))
            continue;

          // check for section name
          const char* name =  elf_strptr (elf, shstrndx, shdr->sh_name);
          if (name && strncmp (name, ".init.", 6) == 0)
            {
              if (sess.verbose)
                clog << "skipping function '" << funcname << "' base 0x"
                     << hex << addr << dec << " is within section '"
                     << name << "'" << endl;
              return;
            }
        }
    }

  if (probe_has_no_target_variables)
    {
      assert(probe_flavours.size() == 1);
      probe = probe_flavours.begin()->second;
    }
  else
    {

      target_variable_flavour_calculating_visitor flav(*this, scope_die, addr);
      base_probe->body->visit(&flav);

      map<string, dwarf_derived_probe *>::iterator i
	= probe_flavours.find(flav.flavour);

      if (i != probe_flavours.end())
	probe = i->second;
      else
	{
	  probe = new dwarf_derived_probe(scope_die, addr, *this);
	  probe_flavours.insert(make_pair(flav.flavour, probe));
	  results.push_back(probe);
	}

      // Cache result in degenerate case to avoid recomputing.
      if (flav.flavour.empty())
	probe_has_no_target_variables = true;
    }

  probe->add_probe_point(funcname, filename, line, addr, *this);
}




      // The critical determining factor when interpreting a pattern
      // string is, perhaps surprisingly: "presence of a lineno". The
      // presence of a lineno changes the search strategy completely.
      //
      // Compare the two cases:
      //
      //   1. {statement,function}(foo@file.c:lineno)
      //      - find the files matching file.c
      //      - in each file, find the functions matching foo
      //      - query the file for line records matching lineno
      //      - iterate over the line records,
      //        - and iterate over the functions,
      //          - if(haspc(function.DIE, line.addr))
      //            - if looking for statements: probe(lineno.addr)
      //            - if looking for functions: probe(function.{entrypc,return,etc.})
      //
      //   2. {statement,function}(foo@file.c)
      //      - find the files matching file.c
      //      - in each file, find the functions matching foo
      //        - probe(function.{entrypc,return,etc.})
      //
      // Thus the first decision we make is based on the presence of a
      // lineno, and we enter entirely different sets of callbacks
      // depending on that decision.


static void
query_statement (string const & func,
		 char const * file,
		 int line,
		 Dwarf_Die *scope_die,
		 Dwarf_Addr stmt_addr,
		 dwarf_query * q)
{
  try
    {
      // XXX: implement
      if (q->has_relative)
        throw semantic_error("incomplete: do not know how to interpret .relative",
                             q->base_probe->tok);

      q->add_probe_point(func, file, line, scope_die, stmt_addr);
    }
  catch (const semantic_error& e)
    {
      q->sess.print_error (e);
    }
}

static void
query_inline_instance_info (Dwarf_Addr entrypc,
			    inline_instance_info & ii,
			    dwarf_query * q)
{
  try
    {
      if (q->has_return)
	{
	  throw semantic_error ("cannot probe .return of inline function '" + ii.name + "'");
	}
      else
	{
	  if (q->sess.verbose)
	    clog << "querying entrypc "
		 << hex << entrypc << dec
		 << " of instance of inline '" << ii.name << "'" << endl;
	  query_statement (ii.name, ii.decl_file, ii.decl_line,
			   &ii.die, entrypc, q);
	}
    }
  catch (semantic_error &e)
    {
      q->sess.print_error (e);
    }
}

static void
query_func_info (Dwarf_Addr entrypc,
		 func_info & fi,
		 dwarf_query * q)
{
  try
    {
      if (q->has_return)
	{
	  // NB. dwarf_derived_probe::emit_registrations will emit a
	  // kretprobe based on the entrypc in this case.
	  if (q->sess.verbose)
	    clog << "querying entrypc of function '"
		 << fi.name << "' for return probe" << endl;
	  query_statement (fi.name, fi.decl_file, fi.decl_line,
			   &fi.die, entrypc, q);
	}
      else
	{
#ifdef __ia64__
	// In IA64 platform function probe point is set at its
	// entry point rather than prologue end pointer
	if (q->sess.verbose)
	   clog << "querying entrypc of function '"
		<< fi.name << "'" << endl;
	   query_statement (fi.name, fi.decl_file, fi.decl_line,
		&fi.die, entrypc, q);

#else
	  if (q->sess.verbose)
	    clog << "querying prologue-end of function '"
		 << fi.name << "'" << endl;

	  if (fi.prologue_end == 0)
	    throw semantic_error("could not find prologue-end "
				 "for probed function '" + fi.name + "'");

	  query_statement (fi.name, fi.decl_file, fi.decl_line,
			   &fi.die, fi.prologue_end, q);
#endif
	}
    }
  catch (semantic_error &e)
    {
      q->sess.print_error (e);
    }
}


static void
query_srcfile_line (Dwarf_Line * line, void * arg)
{
  dwarf_query * q = static_cast<dwarf_query *>(arg);

  Dwarf_Addr addr;
  dwarf_lineaddr(line, &addr);

  for (map<Dwarf_Addr, func_info>::iterator i = q->filtered_functions.begin();
       i != q->filtered_functions.end(); ++i)
    {
      if (q->dw.die_has_pc (&(i->second.die), addr))
	{
	  if (q->sess.verbose)
	    clog << "function DIE lands on srcfile" << endl;
	  if (q->has_statement_str)
	    query_statement (i->second.name, i->second.decl_file,
			     q->line, NULL, addr, q);
	  else
	    query_func_info (i->first, i->second, q);
	}
    }

  for (map<Dwarf_Addr, inline_instance_info>::iterator i
	 = q->filtered_inlines.begin();
       i != q->filtered_inlines.end(); ++i)
    {
      if (q->dw.die_has_pc (&(i->second.die), addr))
	{
	  if (q->sess.verbose)
	    clog << "inline instance DIE lands on srcfile" << endl;
	  if (q->has_statement_str)
	    query_statement (i->second.name, i->second.decl_file,
			     q->line, NULL, addr, q);
	  else
	    query_inline_instance_info (i->first, i->second, q);
	}
    }
}


static int
query_dwarf_inline_instance (Dwarf_Die * die, void * arg)
{
  dwarf_query * q = static_cast<dwarf_query *>(arg);
  assert (!q->has_statement_num);

  try
    {

      bool record_this_inline = false;

      if (q->sess.verbose)
	clog << "examining inline instance of " << q->dw.function_name << endl;

      if (q->has_inline_str || q->has_statement_str)
	record_this_inline = true;

      else if (q->has_inline_num)
	{
	  Dwarf_Addr query_addr = q->inline_num_val;

	  if (q->has_module)
	    query_addr = q->dw.module_address_to_global(query_addr);

	  if (q->dw.die_has_pc (die, query_addr))
	    record_this_inline = true;
	}

      if (record_this_inline)
	{
	  if (q->sess.verbose)
	    clog << "selected inline instance of " << q->dw.function_name << endl;

	  Dwarf_Addr entrypc;
	  if (q->dw.die_entrypc (die, &entrypc))
	    {
	      inline_instance_info inl;
	      inl.die = *die;
	      inl.name = q->dw.function_name;
	      q->dw.function_file (&inl.decl_file);
	      q->dw.function_line (&inl.decl_line);
	      q->filtered_inlines[entrypc] = inl;
	    }
	}
      return DWARF_CB_OK;
    }
  catch (const semantic_error& e)
    {
      q->sess.print_error (e);
      return DWARF_CB_ABORT;
    }
}

static int
query_dwarf_func (Dwarf_Die * func, void * arg)
{
  dwarf_query * q = static_cast<dwarf_query *>(arg);
  assert (!q->has_statement_num);

  try
    {
      // XXX: implement
      if (q->has_callees)
        throw semantic_error ("incomplete: do not know how to interpret .callees",
			      q->base_probe->tok);

      if (q->has_label)
        throw semantic_error ("incomplete: do not know how to interpret .label",
			      q->base_probe->tok);

      q->dw.focus_on_function (func);

      if (q->dw.func_is_inline ()
	  && (((q->has_statement_str || q->has_inline_str)
	       && q->dw.function_name_matches(q->function))
	      || q->has_inline_num))
	{
	  if (q->sess.verbose)
	    clog << "checking instances of inline " << q->dw.function_name << endl;
	  q->dw.iterate_over_inline_instances (query_dwarf_inline_instance, arg);
	}
      else if (!q->dw.func_is_inline ())
	{
	  bool record_this_function = false;

	  if ((q->has_statement_str || q->has_function_str)
	      && q->dw.function_name_matches(q->function))
	    {
	      record_this_function = true;
	    }
	  else if (q->has_function_num)
	    {
	      Dwarf_Addr query_addr = q->function_num_val;

	      if (q->has_module)
		query_addr = q->dw.module_address_to_global(query_addr);

	      Dwarf_Die d;
	      q->dw.function_die (&d);

	      if (q->dw.die_has_pc (&d, query_addr))
		record_this_function = true;
	    }

	  if (record_this_function)
	    {
	      if (q->sess.verbose)
		clog << "selected function " << q->dw.function_name << endl;

	      Dwarf_Addr entrypc;
	      if (q->dw.function_entrypc (&entrypc))
		{
		  func_info func;
		  q->dw.function_die (&func.die);
		  func.name = q->dw.function_name;
		  q->dw.function_file (&func.decl_file);
		  q->dw.function_line (&func.decl_line);
		  q->filtered_functions[entrypc] = func;
		}
	      else
		throw semantic_error("no entrypc found for function '"
				     + q->dw.function_name + "'");
	    }
	}
      return DWARF_CB_OK;
    }
  catch (const semantic_error& e)
    {
      q->sess.print_error (e);
      return DWARF_CB_ABORT;
    }
}

static int
query_cu (Dwarf_Die * cudie, void * arg)
{
  dwarf_query * q = static_cast<dwarf_query *>(arg);

  try
    {
      q->dw.focus_on_cu (cudie);

      if (false && q->sess.verbose)
        clog << "focused on CU '" << q->dw.cu_name
             << "', in module '" << q->dw.module_name << "'" << endl;

      if (q->has_statement_str
	  || q->has_inline_str || q->has_inline_num
	  || q->has_function_str || q->has_function_num)
	{
	  q->filtered_srcfiles.clear();
	  q->filtered_functions.clear();
	  q->filtered_inlines.clear();

	  // In this path, we find "abstract functions", record
	  // information about them, and then (depending on lineno
	  // matching) possibly emit one or more of the function's
	  // associated addresses. Unfortunately the control of this
	  // cannot easily be turned inside out.

	  if ((q->has_statement_str || q->has_function_str || q->has_inline_str)
	      && (q->spec_type != function_alone))
	    {
	      // If we have a pattern string with a filename, we need
	      // to elaborate the srcfile mask in question first.
	      q->dw.collect_srcfiles_matching (q->file, q->filtered_srcfiles);

	      // If we have a file pattern and *no* srcfile matches, there's
	      // no need to look further into this CU, so skip.
	      if (q->filtered_srcfiles.empty())
		return DWARF_CB_OK;
	    }

	  // Pick up [entrypc, name, DIE] tuples for all the functions
	  // matching the query, and fill in the prologue endings of them
	  // all in a single pass.
	  q->dw.iterate_over_functions (query_dwarf_func, q);
          if (! q->filtered_functions.empty()) // No functions in this CU to worry about?
            {
              q->dw.resolve_prologue_endings (q->filtered_functions);
              q->dw.resolve_prologue_endings2 (q->filtered_functions);
            }

	  if ((q->has_statement_str || q->has_function_str || q->has_inline_str)
	      && (q->spec_type == function_file_and_line))
	    {
	      // If we have a pattern string with target *line*, we
	      // have to look at lines in all the matched srcfiles.
	      for (set<char const *>::const_iterator i = q->filtered_srcfiles.begin();
		   i != q->filtered_srcfiles.end(); ++i)
		q->dw.iterate_over_srcfile_lines (*i, q->line, q->has_statement_str,
						  query_srcfile_line, q);
	    }
	  else
	    {
	      // Otherwise, simply probe all resolved functions (if
	      // we're scanning functions)
	      if (q->has_statement_str || q->has_function_str || q->has_function_num)
		for (map<Dwarf_Addr, func_info>::iterator i = q->filtered_functions.begin();
		     i != q->filtered_functions.end(); ++i)
		  query_func_info (i->first, i->second, q);

	      // Or all inline instances (if we're scanning inlines)
	      if (q->has_statement_str || q->has_inline_str || q->has_inline_num)
		for (map<Dwarf_Addr, inline_instance_info>::iterator i
		       = q->filtered_inlines.begin(); i != q->filtered_inlines.end(); ++i)
		  query_inline_instance_info (i->first, i->second, q);

	    }
	}
      else
        {
	  // Otherwise we have a statement number, and we can just
	  // query it directly within this module.

	  assert (q->has_statement_num);
	  Dwarf_Addr query_addr = q->statement_num_val;
	  if (q->has_module)
	    query_addr = q->dw.module_address_to_global(query_addr);

	  query_statement ("", "", -1, NULL, query_addr, q);
        }
      return DWARF_CB_OK;
    }
  catch (const semantic_error& e)
    {
      q->sess.print_error (e);
      return DWARF_CB_ABORT;
    }
}


static int
query_kernel_exists (Dwfl_Module *mod __attribute__ ((unused)),
		     void **userdata __attribute__ ((unused)),
		     const char *name,
		     Dwarf_Addr base __attribute__ ((unused)),
		     void *arg)
{
  int *flagp = (int *) arg;
  if (TOK_KERNEL == name)
    *flagp = 1;
  return DWARF_CB_OK;
}


static int
query_module (Dwfl_Module *mod __attribute__ ((unused)),
	      void **userdata __attribute__ ((unused)),
	      const char *name, Dwarf_Addr base,
	      void *arg __attribute__ ((unused)))
{
  dwarf_query * q = static_cast<dwarf_query *>(arg);

  try
    {
      q->dw.focus_on_module(mod);

      // If we have enough information in the pattern to skip a module and
      // the module does not match that information, return early.

      if (q->has_kernel && !q->dw.module_name_matches(TOK_KERNEL))
        return DWARF_CB_OK;

      if (q->has_module && !q->dw.module_name_matches(q->module_val))
        return DWARF_CB_OK;

      if (q->sess.verbose)
	clog << "focused on module '" << q->dw.module_name
	     << "' = [" << hex << q->dw.module_start
	     << "-" << q->dw.module_end
	     << ", bias " << q->dw.module_bias << "]" << dec << endl;

      if (q->has_inline_num || q->has_function_num || q->has_statement_num)
        {
          // If we have module("foo").function(0xbeef) or
          // module("foo").statement(0xbeef), the address is relative
          // to the start of the module, so we seek the function
          // number plus the module's bias.

          Dwarf_Addr addr;
          if (q->has_function_num)
            addr = q->function_num_val;
          else if (q->has_inline_num)
            addr = q->inline_num_val;
          else
            addr = q->statement_num_val;

	  // NB: We should not have kernel.* here; global addresses
	  // should have bypassed query_module in dwarf_builder::build
	  // and gone directly to query_cu.

          assert (!q->has_kernel);
          assert (q->has_module);
	  q->dw.query_cu_containing_module_address(addr, q);
        }
      else
        {
          // Otherwise if we have a function("foo") or statement("foo")
          // specifier, we have to scan over all the CUs looking for
          // the function(s) in question

          assert(q->has_function_str || q->has_inline_str || q->has_statement_str);
          q->dw.iterate_over_cus(&query_cu, q);

	  // If we just processed the module "kernel", and the user asked for
	  // the kernel pattern, there's no need to iterate over any further
	  // modules

	  if (q->has_kernel && q->dw.module_name_matches(TOK_KERNEL))
	    return DWARF_CB_ABORT;
        }

      return DWARF_CB_OK;
    }
  catch (const semantic_error& e)
    {
      q->sess.print_error (e);
      return DWARF_CB_ABORT;
    }
}


struct
var_expanding_copy_visitor
  : public deep_copy_visitor
{
  static unsigned tick;
  stack<functioncall**> target_symbol_setter_functioncalls;

  dwarf_query & q;
  Dwarf_Die *scope_die;
  Dwarf_Addr addr;

  var_expanding_copy_visitor(dwarf_query & q, Dwarf_Die *sd, Dwarf_Addr a)
    : q(q), scope_die(sd), addr(a)
  {}
  void visit_assignment (assignment* e);
  void visit_target_symbol (target_symbol* e);
};


unsigned var_expanding_copy_visitor::tick = 0;

void
var_expanding_copy_visitor::visit_assignment (assignment* e)
{
  // Our job would normally be to require() the left and right sides
  // into a new assignment. What we're doing is slightly trickier:
  // we're pushing a functioncall** onto a stack, and if our left
  // child sets the functioncall* for that value, we're going to
  // assume our left child was a target symbol -- transformed into a
  // set_target_foo(value) call, and it wants to take our right child
  // as the argument "value".
  //
  // This is why some people claim that languages with
  // constructor-decomposing case expressions have a leg up on
  // visitors.

  functioncall *fcall = NULL;
  expression *new_left, *new_right;

  target_symbol_setter_functioncalls.push (&fcall);
  require<expression*> (this, &new_left, e->left);
  target_symbol_setter_functioncalls.pop ();
  require<expression*> (this, &new_right, e->right);

  if (fcall != NULL)
    {
      // Our left child is informing us that it was a target variable
      // and it has been replaced with a set_target_foo() function
      // call; we are going to provide that function call -- with the
      // right child spliced in as sole argument -- in place of
      // ourselves, in the deep copy we're in the middle of making.

      // FIXME: for the time being, we only support plan $foo = bar,
      // not += or any other op= variant. This is fixable, but a bit
      // ugly.
      if (e->op != "=")
	throw semantic_error ("Operator-assign expressions on target "
			     "variables not implemented", e->tok);

      assert (new_left == fcall);
      fcall->args.push_back (new_right);
      provide <expression*> (this, fcall);
    }
  else
    {
      assignment* n = new assignment;
      n->op = e->op;
      n->tok = e->tok;
      n->left = new_left;
      n->right = new_right;
      provide <assignment*> (this, n);
    }
}


void
var_expanding_copy_visitor::visit_target_symbol (target_symbol *e)
{
  assert(e->base_name.size() > 0 && e->base_name[0] == '$');

  // Synthesize a function.
  functiondecl *fdecl = new functiondecl;
  embeddedcode *ec = new embeddedcode;
  ec->tok = e->tok;
  bool lvalue = is_active_lvalue(e);

  if (lvalue && !q.sess.guru_mode)
    throw semantic_error("Illegal target variable access", e->tok);

  string fname = (string(lvalue ? "set" : "get")
		  + "_" + e->base_name.substr(1)
		  + "_" + lex_cast<string>(tick++));

  try
    {
      ec->code = q.dw.literal_stmt_for_local (scope_die,
					      addr,
					      e->base_name.substr(1),
					      e->components,
					      lvalue,
					      fdecl->type);
    }
  catch (const semantic_error& er)
    {
      // No need to be verbose: the flavour-gathering visitor
      // already printed a message for this exact case.
      throw semantic_error ("due to failed target variable resolution");
    }

  fdecl->name = fname;
  fdecl->body = ec;
  if (lvalue)
    {
      // Modify the fdecl so it carries a single pe_long formal
      // argument called "value".

      // FIXME: For the time being we only support setting target
      // variables which have base types; these are 'pe_long' in
      // stap's type vocabulary.  Strings and pointers might be
      // reasonable, some day, but not today.

      vardecl *v = new vardecl;
      v->type = pe_long;
      v->name = "value";
      v->tok = e->tok;
      fdecl->formal_args.push_back(v);
    }
  q.sess.functions.push_back(fdecl);

  // Synthesize a functioncall.
  functioncall* n = new functioncall;
  n->tok = e->tok;
  n->function = fname;
  n->referent = NULL;

  if (lvalue)
    {
      // Provide the functioncall to our parent, so that it can be
      // used to substitute for the assignment node immediately above
      // us.
      assert(!target_symbol_setter_functioncalls.empty());
      *(target_symbol_setter_functioncalls.top()) = n;
    }

  provide <functioncall*> (this, n);
}


void
dwarf_derived_probe::add_probe_point(string const & funcname,
				     char const * filename,
				     int line,
				     Dwarf_Addr addr,
				     dwarf_query & q)
{
  string module_name(q.dw.module_name);

  // "Adding a probe point" means two things:
  //
  //
  //  1. Adding an addr to the probe-point vector

  probe_points.push_back(addr);

  //  2. Extending the "locations" vector

  vector<probe_point::component*> comps;
  comps.push_back
    (module_name == TOK_KERNEL
     ? new probe_point::component(TOK_KERNEL)
     : new probe_point::component(TOK_MODULE, new literal_string(module_name)));

  string fn_or_stmt;
  if (q.has_function_str || q.has_function_num)
    fn_or_stmt = "function";
  else if (q.has_inline_str || q.has_inline_num)
    fn_or_stmt = "inline";
  else
    fn_or_stmt = "statement";

  if (q.has_function_str || q.has_inline_str || q.has_statement_str)
      {
        string retro_name = funcname;
	if (filename && !string (filename).empty())
	  retro_name += ("@" + string (filename));
	if (line != -1)
	  retro_name += (":" + lex_cast<string> (line));
        comps.push_back
          (new probe_point::component
           (fn_or_stmt, new literal_string (retro_name)));
      }
  else if (q.has_function_num || q.has_inline_num || q.has_statement_num)
    {
      Dwarf_Addr retro_addr;
      if (q.has_function_num)
        retro_addr = q.function_num_val;
      else if (q.has_inline_num)
	retro_addr = q.inline_num_val;
      else
        retro_addr = q.statement_num_val;

      comps.push_back (new probe_point::component
                       (fn_or_stmt,
                        new literal_number(retro_addr))); // XXX: should be hex if possible
    }

  if (has_return)
    comps.push_back
      (new probe_point::component(TOK_RETURN));

  assert(q.base_probe->locations.size() > 0);
  locations.push_back(new probe_point(comps, q.base_probe->locations[0]->tok));
}

dwarf_derived_probe::dwarf_derived_probe (Dwarf_Die *scope_die,
					  Dwarf_Addr addr,
					  dwarf_query & q)
  : derived_probe (NULL),
    has_return (q.has_return)
{
  string module_name(q.dw.module_name);

  // Lock the kernel module in memory.
  if (module_name != TOK_KERNEL)
    {
      // XXX: There is a race window here, between the time that libdw
      // opened up this same file for its relocation duties, and now.
      int fd = q.sess.module_fds[module_name];
      if (fd == 0)
        {
          string sys_module = "/sys/module/" + module_name + "/sections/.text";
          fd = open (sys_module.c_str(), O_RDONLY);
          if (fd < 0)
            throw semantic_error ("error opening module refcount-bumping file.");
          q.sess.module_fds[module_name] = fd;
        }
    }

  // Now make a local-variable-expanded copy of the probe body
  var_expanding_copy_visitor v (q, scope_die, addr);
  require <block*> (&v, &(this->body), q.base_probe->body);
  this->tok = q.base_probe->tok;
}

void
dwarf_derived_probe::register_relative_variants(match_node * root,
						dwarf_builder * dw)
{
  // Here we match 2 forms:
  //
  // .
  // .relative(NN)

  root->bind(dw);
  root->bind_num(TOK_RELATIVE)->bind(dw);
}

void
dwarf_derived_probe::register_statement_variants(match_node * root,
						 dwarf_builder * dw)
{
  // Here we match 3 forms:
  //
  // .
  // .return
  // .label("foo")

  register_relative_variants(root, dw);
  register_relative_variants(root->bind_str(TOK_LABEL), dw);
}

void
dwarf_derived_probe::register_inline_variants(match_node * root,
					      dwarf_builder * dw)
{
  // Here we match 4 forms:
  //
  // .
  // .callees
  // .callees(N)
  //
  // The last form permits N-level callee resolving without any
  // recursive .callees.callees.callees... pattern-matching on our part.

  root->bind(dw);
  root->bind(TOK_CALLEES)->bind(dw);
  root->bind_num(TOK_CALLEES)->bind(dw);
}

void
dwarf_derived_probe::register_function_variants(match_node * root,
					      dwarf_builder * dw)
{
  // Here we match 4 forms:
  //
  // .
  // .return
  // .callees
  // .callees(N)
  //
  // The last form permits N-level callee resolving without any
  // recursive .callees.callees.callees... pattern-matching on our part.

  root->bind(dw);
  root->bind(TOK_RETURN)->bind(dw);
  root->bind(TOK_CALLEES)->bind(dw);
  root->bind_num(TOK_CALLEES)->bind(dw);
}

void
dwarf_derived_probe::register_function_and_statement_variants(match_node * root,
							      dwarf_builder * dw)
{
  // Here we match 4 forms:
  //
  // .function("foo")
  // .function(0xdeadbeef)
  // .inline("foo")
  // .inline(0xdeadbeef)
  // .statement("foo")
  // .statement(0xdeadbeef)

  register_function_variants(root->bind_str(TOK_FUNCTION), dw);
  register_function_variants(root->bind_num(TOK_FUNCTION), dw);
  register_inline_variants(root->bind_str(TOK_INLINE), dw);
  register_inline_variants(root->bind_num(TOK_INLINE), dw);
  register_statement_variants(root->bind_str(TOK_STATEMENT), dw);
  register_statement_variants(root->bind_num(TOK_STATEMENT), dw);
}

void
dwarf_derived_probe::register_patterns(match_node * root)
{
  dwarf_builder *dw = new dwarf_builder();

  // Here we match 3 forms:
  //
  // .kernel
  // .module("foo")
  // .process("foo")

  register_function_and_statement_variants(root->bind(TOK_KERNEL), dw);
  register_function_and_statement_variants(root->bind_str(TOK_MODULE), dw);
  // register_function_and_statement_variants(root->bind_str(TOK_PROCESS), dw);
}

static string
function_name(unsigned probenum)
{
  return "dwarf_kprobe_" + lex_cast<string>(probenum) + "_enter";
}

static string
struct_kprobe_array_name(unsigned probenum)
{
  return "dwarf_kprobe_" + lex_cast<string>(probenum);
}


static string
string_array_name(unsigned probenum)
{
  return "dwarf_kprobe_" + lex_cast<string>(probenum) + "_location_names";
}


void
dwarf_derived_probe::emit_registrations (translator_output* o,
                                         unsigned probenum)
{
  string func_name = function_name(probenum);
  o->newline() << "{";
  o->newline(1) << "int i;";
  o->newline() << "for (i = 0; i < " << probe_points.size() << "; i++) {";
  o->indent(1);
  string probe_name = struct_kprobe_array_name(probenum) + "[i]";

  if (has_return)
    {
      o->newline() << "#ifdef ARCH_SUPPORTS_KRETPROBES";
      o->newline() << probe_name << ".handler = &" << func_name << ";";
      o->newline() << probe_name << ".maxactive = 1;";
      // XXX: pending PR 1289
      // o->newline() << probe_name << ".kp_fault_handler = &stap_kprobe_fault_handler;";
      o->newline() << "rc = register_kretprobe (&(" << probe_name << "));";
      o->newline() << "#else";
      o->newline() << "rc = 1;";
      o->newline() << "#endif";
    }
  else
    {
      o->newline() << probe_name << ".pre_handler = &" << func_name << ";";
      // XXX: pending PR 1289
      // o->newline() << probe_name << ".kp_fault_handler = &stap_kprobe_fault_handler;";
      o->newline() << "rc = register_kprobe (&(" << probe_name << "));";
    }

  o->newline() << "if (unlikely (rc)) {";
  o->newline(1) << "probe_point = " << string_array_name (probenum) << "[i];";
  o->newline() << "break;";
  o->newline(-1) << "}";
  o->newline(-1) << "}";

  // if one failed, must roll back completed registations for this probe
  o->newline() << "if (unlikely (rc)) while (--i >= 0)";
  o->indent(1);
  if (has_return)
    {
      o->newline() << "#ifdef ARCH_SUPPORTS_KRETPROBES";
      o->newline() << "unregister_kretprobe (&(" << probe_name << "));";
      o->newline() << "#else";
      o->newline() << ";";
      o->newline() << "#endif";
    }
  else
    o->newline() << "unregister_kprobe (&(" << probe_name << "));";
  o->newline(-2) << "}";
}


void
dwarf_derived_probe::emit_deregistrations (translator_output* o, unsigned probenum)
{
  o->newline() << "{";
  o->newline(1) << "int i;";
  o->newline() << "for (i = 0; i < " << probe_points.size() << "; i++)";
  string probe_name = struct_kprobe_array_name(probenum) + "[i]";
  o->indent(1);
  if (has_return)
    {
      o->newline() << "#ifdef ARCH_SUPPORTS_KRETPROBES";
      o->newline() << "unregister_kretprobe (&(" << probe_name << "));";
      o->newline() << "#else";
      o->newline() << ";";
      o->newline() << "#endif";
    }
  else
    o->newline() << "unregister_kprobe (&(" << probe_name << "));";
  o->indent(-1);
  o->newline(-1) << "}";
}

void
dwarf_derived_probe::emit_probe_entries (translator_output* o,
                                         unsigned probenum)
{
  static unsigned already_emitted_fault_handler = 0;

  if (! already_emitted_fault_handler)
    {
      o->newline() << "int stap_kprobe_fault_handler (struct kprobe* kp, "
		   << "struct pt_regs* regs, int trapnr) {";
      o->newline(1) << "struct context *c = & contexts [smp_processor_id()];";
      o->newline() << "printk (KERN_ERR \"systemtap probe fault\\n\");";
      o->newline() << "printk (KERN_ERR \"cpu %d, probe %s, near %s\\n\", ";
      o->newline(1) << "smp_processor_id(), ";
      o->newline() << "c->probe_point ? c->probe_point : \"unknown\", ";
      o->newline() << "c->last_stmt ? c->last_stmt : \"unknown\");";
      o->newline() << "c->last_error = \"probe faulted\";";
      o->newline(-1) << "atomic_set (& session_state, STAP_SESSION_ERROR);";

      o->newline() << "return 0;"; // defer to kernel fault handler
      // NB: We might prefer to use "return 1" instead, to consider
      // the fault "handled".  But we may get into an infinite loop
      // of traps if the faulting instruction is simply restarted.

      o->newline(-1) << "}";
      already_emitted_fault_handler ++;
    }


  // Emit arrays of probes and location names.

  string probe_array = struct_kprobe_array_name(probenum);
  string string_array = string_array_name(probenum);

  assert(locations.size() == probe_points.size());

  if (has_return)
    {
      o->newline() << "#ifdef ARCH_SUPPORTS_KRETPROBES";
      o->newline() << "static struct kretprobe "
                   << probe_array
		   << "[" << probe_points.size() << "]"
                   << "= {";
    }
  else
      o->newline() << "static struct kprobe "
                   << probe_array
		   << "[" << probe_points.size() << "]"
                   << "= {";

  o->indent(1);
  for (vector<Dwarf_Addr>::const_iterator i = probe_points.begin();
       i != probe_points.end();
       ++i)
    {
      if (i != probe_points.begin())
        o->line() << ",";
      if (has_return)
        o->newline() << "{.kp.addr= (void *) 0x" << hex << *i << dec << "}";
      else
        o->newline() << "{.addr= (void *) 0x" << hex << *i << dec << "}";
    }
  o->newline(-1) << "};";

  if (has_return)
    o->newline() << "#endif /* ARCH_SUPPORTS_KRETPROBES */";

  o->newline();

  // This is somewhat gross, but it should work: we allocate a
  // *parallel* array of strings containing the location of each
  // probe. You can calculate which kprobe or kretprobe you're in by
  // taking the difference of the struct kprobe pointer and the base
  // of the kprobe array and dividing by the size of the struct kprobe
  // (or kretprobe), then you can use this index into the string table
  // here to work out the *name* of the probe you're in.
  //
  // Sorry.

  assert(probe_points.size() == locations.size());

  o->newline() << "char const * "
	       << string_array
	       << "[" << locations.size() << "] = {";
  o->indent(1);
  for (vector<probe_point*>::const_iterator i = locations.begin();
       i != locations.end(); ++i)
    {
      if (i != locations.begin())
	o->line() << ",";
      o->newline() << lex_cast_qstring(*(*i));
    }
  o->newline(-1) << "};";


  // Construct a single entry function, and a struct kprobe pointing into
  // the entry function. The entry function will call the probe function.
  o->newline();
  if (has_return)
    o->newline() << "#ifdef ARCH_SUPPORTS_KRETPROBES";
  o->newline() << "static int ";
  o->newline() << function_name(probenum) << " (";
  if (has_return)
    o->line() << "struct kretprobe_instance *probe_instance";
  else
    o->line() << "struct kprobe *probe_instance";
  o->line() << ", struct pt_regs *regs) {";
  o->newline(1) << "struct context *c = & contexts [smp_processor_id()];";

  // Calculate the name of the current probe by finding its index in the probe array.
  if (has_return)
    o->newline() << "const char* probe_point = "
		 << string_array
		 << "[ (probe_instance->rp - &(" << probe_array << "[0]))];";
  else
    o->newline() << "const char* probe_point = "
		 << string_array
		 << "[ (probe_instance - &(" << probe_array << "[0]))];";

  // A precondition for running a probe handler is that we're in RUNNING
  // state (not ERROR), and that no one else is already using this context.
  o->newline() << "if (atomic_read (&session_state) != STAP_SESSION_RUNNING)";
  o->newline(1) << "return 0;";
  o->newline(-1) << "if (atomic_inc_return (&c->busy) != 1) {";
  o->newline(1) << "printk (KERN_ERR \"probe reentrancy (%s vs %s)\\n\", "
		    << "c->probe_point, probe_point);";
  o->newline() << "atomic_set (& session_state, STAP_SESSION_ERROR);";
  o->newline() << "atomic_dec (&c->busy);";
  o->newline() << "return 0;";
  o->newline(-1) << "}";
  o->newline();

  o->newline() << "c->last_error = 0;";
  o->newline() << "c->probe_point = probe_point;";
  o->newline() << "c->nesting = 0;";
  o->newline() << "c->regs = regs;";
  o->newline() << "c->actioncount = 0;";

  // NB: locals are initialized by probe function itself
  o->newline() << "probe_" << probenum << " (c);";

  o->newline() << "if (c->last_error && c->last_error[0]) {";
  o->newline(1) << "_stp_error (\"%s near %s\", c->last_error, c->last_stmt);";
  o->newline() << "atomic_set (& session_state, STAP_SESSION_ERROR);";
  o->newline(-1) << "}";

  o->newline() << "atomic_dec (& c->busy);";
  o->newline() << "return 0;";
  o->newline(-1) << "}" << endl;
  if (has_return)
    o->newline() << "#endif /* ARCH_SUPPORTS_KRETPROBES */";

  o->newline();
}


void
dwarf_builder::build(systemtap_session & sess,
		     probe * base,
		     probe_point * location,
		     std::map<std::string, literal *> const & parameters,
		     vector<derived_probe *> & finished_results)
{

  dwflpp dw(sess);
  dwarf_query q(sess, base, location, dw, parameters, finished_results);

  dw.setup(q.has_kernel || q.has_module);

  if (q.has_kernel &&
      (q.has_function_num || q.has_inline_num || q.has_statement_num))
    {
      // If we have kernel.function(0xbeef), or
      // kernel.statement(0xbeef) the address is global (relative to
      // the kernel) and we can seek directly to the module and cudie
      // in question.
      Dwarf_Addr a;
      if (q.has_function_num)
	a = q.function_num_val;
      else if (q.has_inline_num)
	a = q.inline_num_val;
      else
	a = q.statement_num_val;
      dw.focus_on_module_containing_global_address(a);
      dw.query_cu_containing_global_address(a, &q);
    }
  else
    {
      // Otherwise we have module("*bar*"), kernel.statement("foo"), or
      // kernel.function("foo"); in these cases we need to scan all
      // the modules.
      assert((q.has_kernel && q.has_function_str) ||
	     (q.has_kernel && q.has_inline_str) ||
	     (q.has_kernel && q.has_statement_str) ||
	     (q.has_module));
      if (q.has_kernel)
	{
	  int flag = 0;
	  dw.iterate_over_modules(&query_kernel_exists, &flag);
	  if (! flag)
	    throw semantic_error ("cannot find kernel debuginfo");
	}

      dw.iterate_over_modules(&query_module, &q);
    }
}



// ------------------------------------------------------------------------
// timer derived probes
// ------------------------------------------------------------------------


struct timer_derived_probe: public derived_probe
{
  int64_t interval, randomize;

  timer_derived_probe (probe* p, probe_point* l, int64_t i, int64_t r);

  virtual void emit_registrations (translator_output * o, unsigned i);
  virtual void emit_deregistrations (translator_output * o, unsigned i);
  virtual void emit_probe_entries (translator_output * o, unsigned i);
};


timer_derived_probe::timer_derived_probe (probe* p, probe_point* l, int64_t i, int64_t r):
  derived_probe (p, l), interval (i), randomize (r)
{
  if (interval <= 0 || interval > 1000000) // make i and r fit into plain ints
    throw semantic_error ("invalid interval for jiffies timer");
  // randomize = 0 means no randomization
  if (randomize < 0 || randomize > interval)
    throw semantic_error ("invalid randomize for jiffies timer");

  if (locations.size() != 1)
    throw semantic_error ("expect single probe point");
  // so we don't have to loop over them in the other functions
}


void
timer_derived_probe::emit_registrations (translator_output* o, unsigned j)
{
  o->newline() << "init_timer (& timer_" << j << ");";
  o->newline() << "timer_" << j << ".expires = jiffies + " << interval << ";";
  o->newline() << "timer_" << j << ".function = & enter_" << j << ";";
  o->newline() << "add_timer (& timer_" << j << ");";
}


void
timer_derived_probe::emit_deregistrations (translator_output* o, unsigned j)
{
  o->newline() << "del_timer_sync (& timer_" << j << ");";
}


void
timer_derived_probe::emit_probe_entries (translator_output* o, unsigned j)
{
  o->newline() << "static struct timer_list timer_" << j << ";";

  o->newline() << "void enter_" << j << " (unsigned long val) {";
  o->newline(1) << "struct context* c = & contexts [smp_processor_id()];";
  o->newline() << "const char* probe_point = "
	       << lex_cast_qstring(*locations[0]) << ";";
  o->newline() << "(void) val;";

  // A precondition for running a probe handler is that we're in
  // RUNNING state (not ERROR), and that no one else is already using
  // this context.
  o->newline() << "if (atomic_read (&session_state) != STAP_SESSION_RUNNING)";
  o->newline(1) << "return;";

  o->newline(-1) << "if (atomic_inc_return (&c->busy) != 1) {";
  o->newline(1) << "printk (KERN_ERR \"probe reentrancy (%s vs %s)\\n\", "
		<< "c->probe_point, probe_point);";
  o->newline() << "atomic_set (& session_state, STAP_SESSION_ERROR);";
  o->newline() << "atomic_dec (&c->busy);";
  o->newline() << "return;";
  o->newline(-1) << "}";
  o->newline();

  o->newline() << "mod_timer (& timer_" << j << ", "
               << "jiffies + " << interval;
  if (randomize)
    o->line() << " + _stp_random_pm(" << randomize << ")";
  o->line() << ");";

  o->newline() << "c->probe_point = probe_point;";
  o->newline() << "c->last_error = 0;";
  o->newline() << "c->nesting = 0;";
  o->newline() << "c->regs = 0;";

  o->newline() << "#ifdef __i386__";   // task_pt_regs is i386-only
  o->newline() << "if (! in_interrupt())";
  o->newline(1) << "c->regs = task_pt_regs (current);";
  o->newline(-1) << "#endif";

  o->newline() << "c->actioncount = 0;";

  // NB: locals are initialized by probe function itself
  o->newline() << "probe_" << j << " (c);";

  o->newline() << "if (c->last_error && c->last_error[0]) {";
  o->newline(1) << "_stp_error (\"%s near %s\", c->last_error, c->last_stmt);";
  o->newline() << "atomic_set (& session_state, STAP_SESSION_ERROR);";
  o->newline(-1) << "}";

  o->newline() << "atomic_dec (&c->busy);";
  o->newline(-1) << "}" << endl;
}


struct timer_builder: public derived_probe_builder
{
  timer_builder() {}
  virtual void build(systemtap_session & sess,
		     probe * base,
		     probe_point * location,
		     std::map<std::string, literal *> const & parameters,
		     vector<derived_probe *> & finished_results)
  {
    int64_t jn, rn;
    bool jn_p, rn_p;

    jn_p = get_param (parameters, "jiffies", jn);
    rn_p = get_param (parameters, "randomize", rn);

    finished_results.push_back(new timer_derived_probe(base, location,
                                                       jn, rn_p ? rn : 0));
  }
};



// ------------------------------------------------------------------------
//  Standard tapset registry.
// ------------------------------------------------------------------------

void
register_standard_tapsets(systemtap_session & s)
{
  // Rudimentary binders for begin and end targets
  s.pattern_root->bind("begin")->bind(new be_builder(true));
  s.pattern_root->bind("end")->bind(new be_builder(false));
  s.pattern_root->bind("timer")->bind_num("jiffies")->bind(new timer_builder());
  s.pattern_root->bind("timer")->bind_num("jiffies")->bind_num("randomize")->bind(new timer_builder());

  // kernel/module parts
  dwarf_derived_probe::register_patterns(s.pattern_root);
}
