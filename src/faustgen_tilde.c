/*
// Copyright (c) 2018 - GRAME CNCM - CICM - ANR MUSICOLL - Pierre Guillot.
// For information on usage and redistribution, and for a DISCLAIMER OF ALL
// WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

#include <m_pd.h>
#include <s_stuff.h> // needed for sys_register_loader()
#include <string.h>
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

// This used to be in s_stuff.h, but not anymore since 0.55.1test1.
int sys_trytoopenone(const char *dir, const char *name, const char* ext,
    char *dirresult, char **nameresult, unsigned int size, int bin);

// ag: I'm not sure what this definition is supposed to do, but this will
// almost certainly cause trouble when compiling against a Pd version that
// uses double precision t_sample values. Disabled for now, which means that
// Faust's default FAUSTFLOAT type will be used (normally float).
#if 0
#define FAUSTFLOAT t_sample
#endif
#ifdef DSPC
#include <faust/dsp/llvm-dsp-c.h>
#else
#include <faust/dsp/llvm-c-dsp.h>
#endif

#include "faust_tilde_ui.h"
#include "faust_tilde_io.h"
#include "faust_tilde_options.h"
#include <assert.h>

#define FAUSTGEN_VERSION_STR "2.2.0"
#define MAXFAUSTSTRING 4096

// ag: GUI update interval for the passive controls (msec). A zero value will
// force updates for each dsp cycle, which should be avoided to reduce cpu
// usage. The default of 40 msecs will give you 25 frames per second which
// should look smooth enough, while keeping cpu usage to a reasonable level.
// The actual cpu usage may vary with different Pd flavors, the number of
// passive controls, and your hardware, though. You may want to try values of
// 100 and even more if the GUI update slows down your system too much. Note
// that in any case this value only affects the generated GUIs, MIDI output is
// still generated for each dsp cycle whenever the corresponding controls
// change their values.
const double gui_update_time = 40;

// keep track of voice controls
typedef struct _faust_voice {
  int num; // current note playing, if any
  FAUSTFLOATX *freq, *gain, *gate;
  struct _faust_voice *next_free, *next_used;
} t_faust_voice;

typedef struct _faust_key {
  int num;
  struct _faust_key *next;
} t_faust_key;

typedef struct _faustgen_tilde
{
    t_object            f_obj;
    llvm_dsp_factory*   f_dsp_factory;
    llvm_dsp*           f_dsp_instance;
    
    float**             f_signal_matrix_single;
    float*              f_signal_aligned_single;
    
    double**            f_signal_matrix_double;
    double*             f_signal_aligned_double;
    
    t_faust_ui_manager* f_ui_manager;
    t_faust_io_manager* f_io_manager;
    t_faust_opt_manager* f_opt_manager;
 
    t_symbol*           f_dsp_name;
    t_clock*            f_clock;
    double              f_clock_time;
    long                f_time;

    bool                f_active;
    t_symbol*           f_activesym;

    bool                f_isdouble;
    bool                f_midiout;
    int                 f_midichan;
    t_channelmask       f_midichanmsk;
    t_symbol*           f_midirecv;
    bool                f_oscout;
    t_symbol*           f_oscrecv;
    t_symbol*           f_instance_name;
    t_symbol*           f_unique_name;
    double              f_next_tick;
    t_canvas*           f_canvas;

    // old-style polyphony
    int                  f_npoly;
    bool                 f_midiin;
    llvm_dsp**           f_dsps;
    t_faust_ui_manager** f_uis;
    t_faust_voice *f_voices, *f_free, *f_used;
    t_faust_key *f_keys;
} t_faustgen_tilde;

static void faust_free_voices(t_faustgen_tilde *x)
{
  if (x->f_voices) {
    freebytes(x->f_voices, x->f_npoly*sizeof(t_faust_voice));
    while (x->f_keys) {
      t_faust_key *next = x->f_keys->next;
      freebytes(x->f_keys, sizeof(t_faust_key));
      x->f_keys = next;
    }
    x->f_voices = x->f_free = x->f_used = NULL;
  }
}

static void faust_new_voices(t_faustgen_tilde *x, int npoly)
{
  // make sure not to leak any memory on these
  if (x->f_voices) faust_free_voices(x);
  x->f_keys = NULL;
  x->f_voices = getzbytes(npoly*sizeof(t_faust_voice));
  if (!x->f_voices) {
    pd_error(x, "faustgen2~: memory allocation failed - voice controls");
    return;
  }
  // Initialize the free and used lists.
  x->f_free = x->f_voices;
  x->f_used = NULL;
  for (int i = 0; i < npoly; i++) {
    // these will be initialized later
    x->f_voices[i].freq = x->f_voices[i].gain = x->f_voices[i].gate = NULL;
    if (i+1 < npoly)
      x->f_voices[i].next_free = x->f_voices+i+1;
    else
      x->f_voices[i].next_free = NULL;
    x->f_voices[i].next_used = NULL;
  }
}

static t_class *faustgen_tilde_class;

// This is basically the same algorithms as in faust_tilde_ui.c, just
// implemented in terms of cloned rather than internal voices.
#define VOICE_STEALING 1
#define MONOPHONIC 1

// simple MTS-like tuning facility (octave-based tunings only for now)
static t_float note2cps(t_faustgen_tilde *x, int num)
{
  t_float f = num;
  // tuning offset in cents
  t_float *tuning = faust_ui_manager_get_tuning(x->f_ui_manager);
  if (tuning) f += tuning[num%12]/100.0;
  // Pd's mtof() function does the rest
  return mtof(f);
}

static FAUSTFLOAT setfaustflt(t_faustgen_tilde *x, FAUSTFLOATX *z, FAUSTFLOAT v)
{
  if (x->f_isdouble)
    return (*(double*)z = v);
  else
    return (*(float*)z = v);
}

static void voices_noteon(t_faustgen_tilde *x, int num, int val, int chan)
{
  //post("noteon %d %d %d", num, val, chan);
#if MONOPHONIC
  if (x->f_npoly == 1) {
    // monophonic/legato mode, like ye synths of old. Here we simply ignore
    // the free and used lists and keep track of which keys are pressed at any
    // one time.
    t_faust_key *n = getbytes(sizeof(t_faust_key));
    if (n) {
      n->num = num;
      n->next = x->f_keys;
      x->f_keys = n;
    } else {
      pd_error(x, "faustgen2~: memory allocation failed - monophony");
    }
    t_faust_voice *v = x->f_voices;
    //post("monophonic: %d", v-x->f_voices);
    if (v->freq) setfaustflt(x, v->freq, note2cps(x, num));
    if (v->gain) setfaustflt(x, v->gain, ((double)val)/127.0);
    if (v->gate) setfaustflt(x, v->gate, 1.0);
    return;
  }
#endif
#if VOICE_STEALING
  if (!x->f_free) {
    // no more voices, let's "borrow" one (we can just grab it from the
    // beginning of the used list, that's the longest sounding note)
    t_faust_voice *u = x->f_used;
    //post("steal: %d", u-x->f_voices);
    if (!u) return; // this can't happen
    x->f_used = u->next_used;
    x->f_free = u;
    u->next_used = u->next_free = NULL;
  }
#endif
  if (x->f_free) {
    // Move this voice to the end of the used list and update the voice
    // controls to kick off the new voice.
    t_faust_voice *v = x->f_free;
    //post("free: %d", v-x->f_voices);
    x->f_free = x->f_free->next_free;
    v->next_free = v->next_used = NULL;
    if (x->f_used) {
      t_faust_voice *u = x->f_used;
      while (u->next_used) u = u->next_used;
      u->next_used = v;
    } else {
      x->f_used = v;
    }
    v->num = num;
    // Simply bypass all checking of control ranges and steps for now. We
    // might want to do something more comprehensive later. Also, having MTS
    // support would be nice. :)
    if (v->freq) setfaustflt(x, v->freq, note2cps(x, num));
    if (v->gain) setfaustflt(x, v->gain, ((double)val)/127.0);
    if (v->gate) setfaustflt(x, v->gate, 1.0);
  }
}

static void voices_noteoff(t_faustgen_tilde *x, int num, int chan)
{
#if MONOPHONIC
  if (x->f_npoly == 1) {
    if (x->f_keys) {
      t_faust_key *n = x->f_keys, *p = NULL;
      while (n && n->num != num) {
        p = n;
        n = n->next;
      }
      if (n) {
        if (p) {
          // not the currently sounding note; simply remove it
          p->next = n->next;
          freebytes(n, sizeof(t_faust_key));
        } else {
          // currently sounding note, change to the previous one (if any)
          t_faust_voice *v = x->f_voices;
          p = n->next;
          freebytes(n, sizeof(t_faust_key));
          if (p) {
            // legato (change to the previous frequency); note that if you
            // want portamento, you'll have to do this in the Faust source
            if (v->freq) setfaustflt(x, v->freq, note2cps(x, p->num));
          } else {
            // note off
            if (v->gate) setfaustflt(x, v->gate, 0.0);
          }
          x->f_keys = p;
        }
      }
    }
    return;
  }
#endif
  t_faust_voice *u = x->f_used, *v = NULL;
  while (u && u->num != num) {
    v = u;
    u = u->next_used;
  }
  if (u) {
    if (v)
      v->next_used = u->next_used;
    else
      x->f_used = u->next_used;
    u->next_free = u->next_used = NULL;
    // Move this voice to the end of the free list and update the gate
    // control to release the voice.
    if (x->f_free) {
      v = x->f_free;
      while (v->next_free) v = v->next_free;
      v->next_free = u;
    } else {
      x->f_free = u;
    }
    if (u->gate) setfaustflt(x, u->gate, 0.0);
  }
}

static void voices_all_notes_off(t_faustgen_tilde *x)
{
#if MONOPHONIC
  if (x->f_npoly == 1) {
    t_faust_voice *v = x->f_voices;
    if (v->gate) setfaustflt(x, v->gate, 0.0);
    while (x->f_keys) {
      t_faust_key *next = x->f_keys->next;
      freebytes(x->f_keys, sizeof(t_faust_key));
      x->f_keys = next;
    }
    return;
  }
#endif
  for (t_faust_voice *u = x->f_used; u; u = u->next_free) {
    if (u->gate) setfaustflt(x, u->gate, 0.0);
    u->next_free = u->next_used;
    u->next_used = NULL;
  }
  // Move all used voices to the end of the free list.
  if (x->f_free) {
    t_faust_voice *v = x->f_free;
    while (v->next_free) v = v->next_free;
    v->next_free = x->f_used;
  } else {
    x->f_free = x->f_used;
  }
  x->f_used = NULL;
}


//////////////////////////////////////////////////////////////////////////////////////////////////
//                                          FAUST INTERFACE                                     //
//////////////////////////////////////////////////////////////////////////////////////////////////

static void faustgen_tilde_delete_instance(t_faustgen_tilde *x)
{
  if (x->f_dsps) {
    for (int i = 0; i < x->f_npoly; i++) {
      deleteCDSPInstance(x->f_dsps[i]);
    }
    free(x->f_dsps);
    x->f_dsps = NULL;
    faust_free_voices(x);
  } else if (x->f_dsp_instance) {
    deleteCDSPInstance(x->f_dsp_instance);
  }
  x->f_dsp_instance = NULL;
}

static void faustgen_tilde_delete_factory(t_faustgen_tilde *x)
{
    faustgen_tilde_delete_instance(x);
    if(x->f_dsp_factory)
    {
        deleteCDSPFactory(x->f_dsp_factory);
    }
    x->f_dsp_factory = NULL;
}

static void faustgen_tilde_compile(t_faustgen_tilde *x)
{
    char const* filepath;
    int dspstate = canvas_suspend_dsp();
    if(!x->f_dsp_name)
    {
        return;
    }
    filepath = faust_opt_manager_get_full_path(x->f_opt_manager, x->f_dsp_name->s_name);
    if(filepath)
    {
        llvm_dsp* instance;
        llvm_dsp_factory* factory;
        char errors[MAXFAUSTSTRING];
        int noptions            = (int)faust_opt_manager_get_noptions(x->f_opt_manager);
        char const** options    = faust_opt_manager_get_options(x->f_opt_manager);
        int npoly = 0; char midi;
        FAUSTFLOATX *freq = NULL, *gain = NULL, *gate = NULL;
        
        factory = createCDSPFactoryFromFile(filepath, noptions, options, "", errors, -1);
        if(strnlen(errors, MAXFAUSTSTRING))
        {
            pd_error(x, "faustgen2~: try to load %s", filepath);
            pd_error(x, "faustgen2~: %s", errors);
            faustgen_tilde_delete_instance(x);
            faustgen_tilde_delete_factory(x);
            canvas_resume_dsp(dspstate);
            return;
        }
        
        instance = createCDSPInstance(factory);
        if(instance)
        {
            const int ninputs = getNumInputsCDSPInstance(instance);
            const int noutputs = getNumOutputsCDSPInstance(instance);
            const bool isdbl = faust_opt_has_double_precision(x->f_opt_manager);
            x->f_isdouble = isdbl;
            logpost(x, 3, "faustgen2~ %s (%d/%d)", x->f_dsp_name->s_name, ninputs, noutputs);
            faust_ui_manager_init(x->f_ui_manager, instance, isdbl, false);
            faust_io_manager_init(x->f_io_manager, ninputs, noutputs);
            
            faustgen_tilde_delete_instance(x);
            faustgen_tilde_delete_factory(x);
            
            x->f_dsp_factory  = factory;
            x->f_dsp_instance = instance;

            /* Register after the JIT initializes its lazy static mutexes. */
            static int exit_cleanup_registered = 0;
            if (!exit_cleanup_registered) {
                atexit(deleteAllCDSPFactories);
                exit_cleanup_registered = 1;
            }

            if (faust_ui_manager_get_polyphony(x->f_ui_manager, &midi, &npoly,
                                               &freq, &gain, &gate)) {
              faust_new_voices(x, npoly);
              if (!x->f_voices) {
                // memory allocation error, error message is already printed
                canvas_resume_dsp(dspstate);
                return;
              }
              //logpost(x, 3, "             npoly = %d (freq = %p, gain = %p, gate = %p)", npoly, freq, gain, gate);
              logpost(x, 3, "             [%d voice polyphony (nvoices:%d)]", npoly, npoly);
              x->f_voices[0].freq = freq;
              x->f_voices[0].gain = gain;
              x->f_voices[0].gate = gate;
              // get rid of the old uis
              if (x->f_uis) {
                for (int i = 1; i < x->f_npoly; i++)
                  faust_ui_manager_free(x->f_uis[i]);
              }
              // clone as many dsps as we need, and clone the uis as we go along
              x->f_dsps = malloc(npoly*sizeof(llvm_dsp*));
              x->f_uis = realloc(x->f_uis, npoly*sizeof(t_faust_ui_manager*));
              assert(x->f_dsps && x->f_uis);
              x->f_dsps[0] = x->f_dsp_instance;
              x->f_uis[0] = x->f_ui_manager;
              for (int i = 1; i < npoly; i++) {
                x->f_dsps[i] = cloneCDSPInstance(x->f_dsp_instance);
                // XXXFIXME: We'd really like to clone the existing ui here:
                //x->f_uis[i] = faust_ui_manager_clone(x->f_ui_manager);
                // That would let us keep as much of the existing control
                // values as possible, as is done with the new-style
                // polyphony. But alas, there's currently no way of doing
                // that, so instead we resort to creating a new ui from
                // scratch.
                x->f_uis[i] = faust_ui_manager_new((t_object*)x);
                faust_ui_manager_init(x->f_uis[i], x->f_dsps[i], isdbl, true);
                char _midi; int _npoly;
                bool ret =
                  faust_ui_manager_get_polyphony(x->f_uis[i], &_midi, &_npoly,
                                                 &freq, &gain, &gate);
                assert(ret && midi == _midi && npoly == _npoly);
                x->f_voices[i].freq = freq;
                x->f_voices[i].gain = gain;
                x->f_voices[i].gate = gate;
              }
              x->f_npoly = npoly;
              x->f_midiin = midi;
            }

            if (x->f_unique_name && x->f_instance_name) {
              // recreate the Pd GUI
              faust_ui_manager_gui(x->f_ui_manager,
                                   x->f_unique_name, x->f_instance_name);
              if (x->f_uis) {
                // also install receivers on the other instances
                for (int i = 1; i < x->f_npoly; i++)
                  faust_ui_manager_gui2(x->f_uis[i],
                                        x->f_unique_name, x->f_instance_name);
              }
            }

            canvas_resume_dsp(dspstate);
            return;
        }
        
        deleteCDSPFactory(factory);
        faustgen_tilde_delete_instance(x);
        faustgen_tilde_delete_factory(x);
        pd_error(x, "faustgen2~: memory allocation failed - instance");
        canvas_resume_dsp(dspstate);
        return;
    }
    pd_error(x, "faustgen2~: source file not found %s", x->f_dsp_name->s_name);
    canvas_resume_dsp(dspstate);
}

static void faustgen_tilde_compile_options(t_faustgen_tilde *x, t_symbol* s, int argc, t_atom* argv)
{
    faust_opt_manager_parse_compile_options(x->f_opt_manager, argc, argv);
    faustgen_tilde_compile(x);
}

#ifdef _WIN32
#include <windows.h>
#endif

/* purr-data support. If this is non-NULL then access to the GUI uses
   JavaScript instead of Tcl/Tk. */
static void (*nw_gui_vmess)(const char *sel, char *fmt, ...) = NULL;

/* New menu-based interface to the editor. */
static void faustgen_tilde_menu_open(t_faustgen_tilde *x)
{
  if (x->f_dsp_instance) {
    const char *pathname = faust_opt_manager_get_full_path(x->f_opt_manager, x->f_dsp_name->s_name);
    if (nw_gui_vmess)
      nw_gui_vmess("open_textfile", "s", pathname);
    else
      sys_vgui("::pd_menucommands::menu_openfile {%s}\n", pathname);
  } else {
    pd_error(x, "faustgen2~: no FAUST DSP file defined");
  }
}

#if 0
/* Old click-based editor interface. This doesn't work very well, as the
   system() call blocks until the editor exits, and filename arguments aren't
   always quoted. Modern Pd offers a better interface which hooks into the
   Open context menu option and doesn't block the caller, see above. */

static void faustgen_tilde_open_texteditor(t_faustgen_tilde *x)
{
    if(x->f_dsp_instance)
    {
        char message[MAXPDSTRING];
#ifdef _WIN32
                char temp[MAXPDSTRING];
        sys_bashfilename(faust_opt_manager_get_full_path(x->f_opt_manager, x->f_dsp_name->s_name), temp);
                sprintf(message, "\"%s\"", temp);
        WinExec(message, SW_HIDE);
        return;
#elif __APPLE__
        sprintf(message, "open -t %s", faust_opt_manager_get_full_path(x->f_opt_manager, x->f_dsp_name->s_name));
        if(system(message))
        {
            
        }
        return;
#else
        sprintf(message, "xdg-open %s", faust_opt_manager_get_full_path(x->f_opt_manager, x->f_dsp_name->s_name));
        if(system(message))
        {
            
        }
        return;
#endif
    }
    pd_error(x, "faustgen2~: no FAUST DSP file defined");
}
#endif

/*
static void faustgen_tilde_read(t_faustgen_tilde *x, t_symbol* s)
{
    x->f_dsp_name = s;
    faustgen_tilde_compile(x);
}
 */

static long faustgen_tilde_get_time(t_faustgen_tilde *x)
{
    if(x->f_dsp_instance)
    {
        struct stat attrib;
        stat(faust_opt_manager_get_full_path(x->f_opt_manager, x->f_dsp_name->s_name), &attrib);
        return attrib.st_ctime;
    }
    return 0;
}

static void faustgen_tilde_autocompile_tick(t_faustgen_tilde *x)
{
    long ntime = faustgen_tilde_get_time(x);
    if(ntime != x->f_time)
    {
        x->f_time = ntime;
        faustgen_tilde_compile(x);
    }
    clock_delay(x->f_clock, x->f_clock_time);
}

static void faustgen_tilde_autocompile(t_faustgen_tilde *x, t_symbol* s, int argc, t_atom* argv)
{
    float state = atom_getfloatarg(0, argc, argv);
    if(fabsf(state) > FLT_EPSILON)
    {
        float time = atom_getfloatarg(1, argc, argv);
        x->f_clock_time = (time > FLT_EPSILON) ? (double)time : 100.;
        x->f_time = faustgen_tilde_get_time(x);
        clock_delay(x->f_clock, x->f_clock_time);
    }
    else
    {
        clock_unset(x->f_clock);
    }
}

static void faustgen_tilde_print(t_faustgen_tilde *x)
{
    if(x->f_dsp_factory)
    {
        post("faustgen2~: %s", faust_opt_manager_get_full_path(x->f_opt_manager, x->f_dsp_name->s_name));
        post("unique name: %s", x->f_unique_name->s_name);
        if (x->f_instance_name)
          post("instance name: %s", x->f_instance_name->s_name);
        faust_io_manager_print(x->f_io_manager, 0);
        if(x->f_dsp_factory)
        {
            char* text = NULL;
            text = getCTarget(x->f_dsp_factory);
            if(text)
            {
                if(strnlen(text, 1) > 0)
                {
                    post("target: %s", text);
                }
                free(text);
            }
            text = getCDSPFactoryCompileOptions(x->f_dsp_factory);
            if(text)
            {
                if(strnlen(text, 1) > 0)
                {
                    post("options: %s", text);
                }
                free(text);
            }
        }
        faust_ui_manager_print(x->f_ui_manager, 0);
    }
    else
    {
        pd_error(x, "faustgen2~: no FAUST DSP file defined");
    }
}

static void out_anything(t_symbol *outsym, t_outlet *out, t_symbol *s, int argc, t_atom *argv)
{
  if (outsym)
    typedmess(outsym->s_thing, s, argc, argv);
  else
    outlet_anything(out, s, argc, argv);
}

static void faustgen_tilde_dump(t_faustgen_tilde *x, t_symbol *outsym)
{
    if (outsym && !*outsym->s_name) outsym = NULL;
    if (outsym && !outsym->s_thing) return;
    if(x->f_dsp_factory) {
      t_outlet *out = faust_io_manager_get_extra_output(x->f_io_manager);
      t_atom argv[1];
      int numparams;
      SETSYMBOL(argv, x->f_dsp_name);
      out_anything(outsym, out, gensym("name"), 1, argv);
      SETSYMBOL(argv, x->f_unique_name);
      out_anything(outsym, out, gensym("unique-name"), 1, argv);
      if (x->f_instance_name) {
        SETSYMBOL(argv, x->f_instance_name);
        out_anything(outsym, out, gensym("instance-name"), 1, argv);
      }
      SETSYMBOL(argv, gensym(faust_opt_manager_get_full_path(x->f_opt_manager, x->f_dsp_name->s_name)));
      out_anything(outsym, out, gensym("path"), 1, argv);
      SETFLOAT(argv, faust_io_manager_get_ninputs(x->f_io_manager));
      out_anything(outsym, out, gensym("numinputs"), 1, argv);
      SETFLOAT(argv, faust_io_manager_get_noutputs(x->f_io_manager));
      out_anything(outsym, out, gensym("numoutputs"), 1, argv);
      if(x->f_dsp_factory) {
        char* text = NULL;
        text = getCTarget(x->f_dsp_factory);
        if(text) {
          if(strnlen(text, 1) > 0) {
            SETSYMBOL(argv, gensym(text));
            out_anything(outsym, out, gensym("target"), 1, argv);
          }
          free(text);
        }
        text = getCDSPFactoryCompileOptions(x->f_dsp_factory);
        if(text) {
          if(strnlen(text, 1) > 0) {
            SETSYMBOL(argv, gensym(text));
            out_anything(outsym, out, gensym("options"), 1, argv);
          }
          free(text);
        }
      }
      numparams = faust_ui_manager_dump(x->f_ui_manager, gensym("param"), out, outsym);
      SETFLOAT(argv, numparams);
      out_anything(outsym, out, gensym("numparams"), 1, argv);
    } else {
      pd_error(x, "faustgen2~: no FAUST DSP file defined");
    }
}

static bool is_blank(const char *s)
{
  while (isblank(*s)) s++;
  return *s == 0;
}

static void faustgen_tilde_tuning(t_faustgen_tilde *x, t_symbol* s, int argc, t_atom* argv)
{
  if (argc <= 0) {
    // output the current tuning on the control outlet
    int ac;
    t_atom av[12];
    t_float *tuning = faust_ui_manager_get_tuning(x->f_ui_manager);
    t_outlet *out = faust_io_manager_get_extra_output(x->f_io_manager);
    if (tuning) {
      ac = 12;
      for (int i = 0; i < ac; i++)
        SETFLOAT(av+i, tuning[i]);
    } else {
      ac = 1;
      // indicates the default (12-tet)
      SETSYMBOL(av, gensym("default"));
    }
    outlet_anything(out, s, ac, av);
    return;
  } else if (argv[0].a_type == A_SYMBOL &&
             (argc == 1 || (argc == 2 && argv[1].a_type == A_FLOAT))) {
    const char *name = argv[0].a_w.w_symbol->s_name, *ext = strrchr(name, '.');
    int base = argc>1?argv[1].a_w.w_float:0;
    if (ext && !strchr(ext, '/'))
      // extension already supplied, no default extension
      ext = "";
    else
      ext = ".scl";
    if (base < 0 || base > 11) {
      pd_error(x, "faustgen2~: wrong 2nd argument to Scala tuning (expected reference tone 0..11)");
      return;
    }
    if (strcmp(name, "default") == 0)
      // reset to the default (12-tet)
      faust_ui_manager_clear_tuning(x->f_ui_manager);
    else {
      // load tuning from a Scala file
      // (http://www.huygens-fokker.org/scala/scl_format.html)
      char realdir[MAXPDSTRING], *realname = NULL;
      int fd = canvas_open(x->f_canvas, name, ext, realdir,
                           &realname, MAXPDSTRING, 0);
      if (fd < 0) {
        pd_error(x, "faustgen2~: can't find %s.scl", name);
        return;
      }
      sys_close(fd);
      char absname[MAXPDSTRING];
      memset(absname, 0, MAXPDSTRING);
      snprintf(absname, MAXPDSTRING, "%s/%s", realdir, realname);
      FILE *fp = fopen(absname, "r");
      if (!fp) {
        pd_error(x, "faustgen2~: can't open %s", realname);
        return;
      }
      size_t lines = 0, state = 0;
      char buf[MAXPDSTRING];
      int n, p, q, pos;
      t_float tuning[12];
      float c;
      while (state < 3 && fgets(buf, MAXPDSTRING, fp)) {
        lines++;
        // remove the trailing newline
        size_t l = strlen(buf);
        if (buf[l-1] == '\n') buf[l-1] = 0;
        // ignore empty and comment lines (comments begin with '!')
        if (*buf && *buf != '!') {
          switch (state) {
          case 0:
            // description
            logpost(x, 3, "%s", buf);
            state++;
            break;
          case 1:
            // scale size, we expect 12 for an octave-based tuning
            if (sscanf(buf, "%d", &n) != 1) {
              pd_error(x, "faustgen2~: %s:%lu: expected scale size", realname, lines);
              return;
            } else if (n != 12) {
              pd_error(x, "faustgen2~: %s:%lu: not an octave-based tuning", realname, lines);
              return;
            }
            n = 0;
            tuning[0] = 0.0; // implicit
            state++;
            break;
          case 2:
            // parsing the scale
            ++n;
            if (sscanf(buf, "%d/%d%n", &p, &q, &pos) == 2 &&
                       is_blank(buf+pos)) {
              // ratio, convert to cent
              if (p > 0 && q > 0) {
                c = 1200.0*log((t_float)p/(t_float)q)/log(2.0);
              } else {
                pd_error(x, "faustgen2~: %s:%lu: invalid ratio", realname, lines);
                return;
              }
            } else if (sscanf(buf, "%g%n", &c, &pos) == 1 && is_blank(buf+pos)) {
              // According to the Scala format, the value we read must contain
              // a period, otherwise it is to be interpreted as a ratio with
              // denominator 1.
              // cf. http://www.huygens-fokker.org/scala/scl_format.html
              if (!strchr(buf, '.')) {
                c = 1200.0*log(c)/log(2.0);
              }
            } else {
              pd_error(x, "faustgen2~: %s:%lu: expected ratio or cent value", realname, lines);
              return;
            }
            c -= n*100.0;
            if (// check for tuning offsets which are wildly out of range
                c < -100.0 || c > 100.0 ||
                // also, the 12th scale point (which isn't part of the tuning
                // table) should be a reasonably exact octave
                (n == 12 && fabs(c) > 1e-8)) {
              pd_error(x, "faustgen2~: %s:%lu: tuning offset out of range", realname, lines);
              return;
            }
            if (n < 12)
              tuning[n] = c;
            else
              // we're done, bail out, ignore any trailing junk for now
              state++;
            break;
          }
        }
      }
      fclose(fp);
      if (base) {
        // shift the scale so that the reference tone is at 0 cent
        t_float r = tuning[base];
        for (int i = 0; i < 12; i++)
          tuning[i] -= r;
      }
      faust_ui_manager_set_tuning(x->f_ui_manager, tuning);
    }
    return;
  } else if (argc == 12) {
    // expect 12 tuning offset values in cents
    bool ok = true;
    t_float tuning[12];
    for (int i = 0; i < argc && ok; i++) {
      if ((ok = (argv[i].a_type == A_FLOAT))) {
        tuning[i] = argv[i].a_w.w_float;
      }
    }
    if (ok) {
      faust_ui_manager_set_tuning(x->f_ui_manager, tuning);
      return;
    }
  }
  pd_error(x, "faustgen2~: wrong arguments to tuning (expected Scala filename or 12 tuning offsets in cent)");
}

static void faustgen_tilde_allnotesoff(t_faustgen_tilde *x)
{
  if (x->f_dsps) {
    voices_all_notes_off(x);
  } else if (x->f_dsp_instance) {
    faust_ui_manager_all_notes_off(x->f_ui_manager);
  }
}

static void faustgen_tilde_defaults(t_faustgen_tilde *x)
{
  if(x->f_dsps) {
    for (int i = 0; i < x->f_npoly; i++) {
      faust_ui_manager_restore_default(x->f_uis[i]);
    }
  } else if (x->f_dsp_instance) {
    faust_ui_manager_restore_default(x->f_ui_manager);
  }
}

static void faustgen_tilde_gui(t_faustgen_tilde *x)
{
  if(x->f_dsp_instance) {
    faust_ui_manager_gui(x->f_ui_manager,
                         x->f_unique_name, x->f_instance_name);
    if (x->f_uis) {
      // also install receivers on the other instances
      for (int i = 1; i < x->f_npoly; i++)
        faust_ui_manager_gui2(x->f_uis[i],
                              x->f_unique_name, x->f_instance_name);
    }
  }
}

static void faustgen_tilde_oscout(t_faustgen_tilde *x, t_symbol* s, int argc, t_atom* argv)
{
  if (argc <= 0)
    // disable OSC receiver
    x->f_oscrecv = NULL;
  else if (argv->a_type == A_FLOAT)
    // toggle OSC output via control outlet
    x->f_oscout = argv->a_w.w_float != 0;
  else if (argv->a_type == A_SYMBOL)
    // enable OSC receiver
    x->f_oscrecv = argv->a_w.w_symbol;
}

static void faustgen_tilde_midiout(t_faustgen_tilde *x, t_symbol* s, int argc, t_atom* argv)
{
  if (argc <= 0)
    // disable MIDI receiver
    x->f_midirecv = NULL;
  else if (argv->a_type == A_FLOAT)
    // toggle MIDI output via control outlet
    x->f_midiout = argv->a_w.w_float != 0;
  else if (argv->a_type == A_SYMBOL)
    // enable MIDI receiver
    x->f_midirecv = argv->a_w.w_symbol;
}

static void add_midichan(t_faustgen_tilde *x, int idx, int c)
{
  if (c == 0) {
    // reset to omni
    x->f_midichanmsk = ALL_CHANNELS;
  } else if (c < 0) {
    if (idx == 0) x->f_midichanmsk = ALL_CHANNELS;
    // block that channel
    if (-c <= 64) x->f_midichanmsk &= ~(1UL << (-c-1));
  } else if (c <= 64) {
    if (idx == 0) x->f_midichanmsk = 0;
    // accept that channel
    x->f_midichanmsk |= 1UL << (c-1);
    // also set the default output channel if it hasn't been set yet
    if (x->f_midichan < 0) x->f_midichan = c-1;
  }
}

static void faustgen_tilde_midichan(t_faustgen_tilde *x, t_symbol* s, int argc, t_atom* argv)
{
  if (argc <= 0) {
    // output the current status
    int ac = 0;
    t_atom av[64];
    t_outlet *out = faust_io_manager_get_extra_output(x->f_io_manager);
    // Make sure that the default channel comes first, if any
    if (x->f_midichan >= 0 && x->f_midichan < 64) {
      SETFLOAT(av+ac, x->f_midichan+1); ac++;
    }
    for (int chan = 0; chan < 64; chan++)
      if (chan != x->f_midichan && ((1UL<<chan) & x->f_midichanmsk) != 0UL) {
        SETFLOAT(av+ac, chan+1); ac++;
      }
    outlet_anything(out, s, ac, av);
  } else {
    t_channelmask oldmsk = x->f_midichanmsk;
    // reset the default channel
    x->f_midichan = -1;
    // default to omni
    x->f_midichanmsk = ALL_CHANNELS;
    for (int i = 0; i < argc; i++) {
      if (argv[i].a_type == A_FLOAT) {
        // set MIDI channel (0 means omni, negative blocks that channel)
        add_midichan(x, i, argv[i].a_w.w_float);
      } else {
        char buf[MAXPDSTRING];
        atom_string(&argv[i], buf, MAXPDSTRING);
        pd_error(x, "faustgen2~: bad midi channel number '%s'", buf);
      }
    }
    if (x->f_midichanmsk != oldmsk)
      // prevent hanging notes after change
      if (x->f_dsps)
        voices_all_notes_off(x);
      else
        faust_ui_manager_all_notes_off(x->f_ui_manager);
  }
}


//////////////////////////////////////////////////////////////////////////////////////////////////
//                                  PURE DATA GENERIC INTERFACE                                 //
//////////////////////////////////////////////////////////////////////////////////////////////////

static bool anything(t_faust_ui_manager *ui, t_symbol* s, int argc, t_atom* argv,
                     t_symbol *recv, t_outlet* oscout, t_outlet* out,
                     t_channelmask midichanmsk, t_faustgen_tilde *x, bool quiet)
{
  const t_symbol *msg_s = faust_ui_manager_get_osc(ui, s, argc, argv, recv, oscout);
  if(msg_s) return true;

  int msg = faust_ui_manager_get_midi(ui, s, argc, argv, midichanmsk);
  if(msg) return true;

  if(!argc)
  {
    t_float value;
    if(!faust_ui_manager_get_value(ui, s, &value))
    {
      t_atom av;
      SETFLOAT(&av, value);
      outlet_anything(out, s, 1, &av);
      return true;
    }
    if (!quiet) pd_error(x, "faustgen2~: parameter '%s' not defined", s->s_name);
    return false;
  }
  else if(argc == 1)
  {
    if(argv[0].a_type != A_FLOAT)
    {
      if (!quiet) pd_error(x, "faustgen2~: parameter requires a float value");
      return false;
    }
    if(!faust_ui_manager_set_value(ui, s, argv[0].a_w.w_float))
    {
      return true;
    }
    if(s == x->f_activesym)
    // ag: default action for 'active' message, toggles the activation
    // status of the dsp
    {
      x->f_active = argv[0].a_w.w_float != 0;
      return true;
    }
    if (!quiet) pd_error(x, "faustgen2~: parameter '%s' not defined", s->s_name);
    return false;
  }
  else
  {
    int i, start;
    char name[MAXFAUSTSTRING];
    if(argv[0].a_type != A_FLOAT)
    {
      pd_error(x, "faustgen2~: list parameters require a first index");
      return false;
    }
    start = (int)argv[0].a_w.w_float;
    for(i = 0; i < argc - 1; ++i)
    {
      snprintf(name, MAXFAUSTSTRING, "%s%i", s->s_name, start+i);
      if(argv[i+1].a_type != A_FLOAT)
      {
        pd_error(x, "faustgen2~: active parameter requires a float value");
      }
      if(faust_ui_manager_set_value(ui, gensym(name), argv[i+1].a_w.w_float))
      {
        pd_error(x, "faustgen2~: active parameter '%s' not defined", name);
        return false;
      }
    }
    return true;
  }
}

static bool midichan_check(t_channelmask msk, int chan)
{
  if (chan >= 0 && chan < 64)
    return ((1UL<<chan) & msk) != 0UL;
  else
    return false;
}

static void faustgen_tilde_anything(t_faustgen_tilde *x, t_symbol* s, int argc, t_atom* argv)
{
  if(x->f_dsps) {
    // multiple instances, old-style polyphony processing
    /* We only handle the note messages here, since we have to dispatch them
       to the right dsp instance using voice allocation. Anything else is simply
       passed on to all of the ui managers. XXXFIXME: It isn't terribly clear
       how to handle the OSC and MIDI output of all the instances. Should we
       aggregate them in some way? Currently we only enable output for the
       first instance and ignore all the rest to prevent duplicated messages. */
    if (strcmp(s->s_name, "note")) {
      // any errors should be caught on the first run
      bool res =
        anything(x->f_ui_manager, s, argc, argv, x->f_oscrecv,
                 x->f_oscout ? faust_io_manager_get_extra_output(x->f_io_manager) : NULL, faust_io_manager_get_extra_output(x->f_io_manager), x->f_midichanmsk, x, false);
      if (res) {
        // now go ahead and feed the same message to all the other instances,
        // silence output
        for (int i = 1; i < x->f_npoly; i++) {
          anything(x->f_uis[i], s, argc, argv, NULL,
                   NULL, NULL, x->f_midichanmsk, x, true);
        }
      }
    } else {
      // handle an SMMF style note message
      int num, val, chan = -1;
      // Check whether MIDI processing is actually on. If it is off, we just
      // silently ignore it.
      if (!x->f_midiin) return;
      // Process the message arguments.
      if (argc < 2) return;
      if (argv[0].a_type != A_FLOAT) return;
      num = (int)argv[0].a_w.w_float;
      if (argv[1].a_type != A_FLOAT) return;
      val = (int)argv[1].a_w.w_float;
      if (argc > 2 && argv[2].a_type == A_FLOAT) {
        // channel argument
        chan = (int)argv[2].a_w.w_float;
        // check validity
        if (chan >= 1) {
          // Subtract 1 since channels are zero-based in Faust meta data, but
          // 1-based in Pd. NOTE: Pd allows more than the usual 16 channels,
          // since it treats each MIDI device as a separate block of 16 MIDI
          // channels. Thus 0..15 will denote the channels of the first MIDI
          // device, 16..31 the channels of the second one, etc.
          chan--;
          // match against the object's channel mask
          if (!midichan_check(x->f_midichanmsk, chan)) return;
        } else
          chan = -1;
      }
      if (val)
        voices_noteon(x, num, val, chan);
      else
        voices_noteoff(x, num, chan);
    }
  }
  else if(x->f_dsp_instance)
  {
    // single instance, new-style polyphony (if any)
    anything(x->f_ui_manager, s, argc, argv, x->f_oscrecv,
             x->f_oscout ? faust_io_manager_get_extra_output(x->f_io_manager) : NULL, faust_io_manager_get_extra_output(x->f_io_manager), x->f_midichanmsk, x, false);
  }
  else
    pd_error(x, "faustgen2~: no dsp instance");
}

static t_int *faustgen_tilde_perform_single(t_int *w)
{
    int i, j;
    llvm_dsp *dsp = (llvm_dsp *)w[1];
    int const nsamples  = (int)w[2];
    int const ninputs   = (int)w[3];
    int const noutputs  = (int)w[4];
    float** faustsigs   = (float **)w[5];
    t_sample const** realinputs = (t_sample const**)w[6];
    t_sample** realoutputs      = (t_sample **)w[7];
    t_faustgen_tilde *x = (t_faustgen_tilde *)w[8];
    if (!x->f_active) {
      // ag: default `active` flag: bypass or mute the dsp
      if (ninputs == noutputs) {
        for (i = 0; i < ninputs; ++i) {
          for(j = 0; j < nsamples; ++j) {
            realoutputs[i][j] = realinputs[i][j];
          }
        }
      } else {
        for (i = 0; i < noutputs; ++i) {
          for(j = 0; j < nsamples; ++j) {
            realoutputs[i][j] = 0.0;
          }
        }
      }
      return (w+9);
    }
    for(i = 0; i < ninputs; ++i)
    {
        for(j = 0; j < nsamples; ++j)
        {
            faustsigs[i][j] = (FAUSTFLOAT)realinputs[i][j];
        }
    }
    computeCDSPInstance(dsp, nsamples, (FAUSTFLOAT**)faustsigs, (FAUSTFLOAT**)(faustsigs+ninputs));
    for(i = 0; i < noutputs; ++i)
    {
        for(j = 0; j < nsamples; ++j)
        {
            realoutputs[i][j] = (t_sample)faustsigs[ninputs+i][j];
        }
    }
    if (x->f_dsps) {
      // sum up the outputs from all dsp instances
      for (int k = 1; k < x->f_npoly; k++) {
        computeCDSPInstance(x->f_dsps[k], nsamples, (FAUSTFLOAT**)faustsigs, (FAUSTFLOAT**)(faustsigs+ninputs));
        for(i = 0; i < noutputs; ++i)
        {
          for(j = 0; j < nsamples; ++j)
          {
            realoutputs[i][j] += (t_sample)faustsigs[ninputs+i][j];
          }
        }
      }
    }
    // XXXFIXME: If we have multiple dsps (in old-style polyphony), we only
    // output MIDI and OSC data from the first instance here, to prevent
    // duplicate messages. Maybe they should be aggregated instead. (See the
    // remarks in faustgen_tilde_anything.)
    if (x->f_midiout || x->f_midirecv) {
      t_outlet *out = x->f_midiout?faust_io_manager_get_extra_output(x->f_io_manager):NULL;
      faust_ui_manager_midiout(x->f_ui_manager, x->f_midichan, x->f_midirecv, out);
    }
    if (clock_getsystime() >= x->f_next_tick) {
      if (x->f_oscout || x->f_oscrecv) {
        t_outlet *out = x->f_oscout?faust_io_manager_get_extra_output(x->f_io_manager):NULL;
        faust_ui_manager_oscout(x->f_ui_manager, x->f_oscrecv, out);
      }
      if (x->f_instance_name && x->f_instance_name->s_thing)
        faust_ui_manager_gui_update(x->f_ui_manager);
      x->f_next_tick = clock_getsystimeafter(gui_update_time);
    }
    return (w+9);
}

static t_int *faustgen_tilde_perform_double(t_int *w)
{
    int i, j;
    llvm_dsp *dsp = (llvm_dsp *)w[1];
    int const nsamples  = (int)w[2];
    int const ninputs   = (int)w[3];
    int const noutputs  = (int)w[4];
    double** faustsigs  = (double **)w[5];
    t_sample const** realinputs = (t_sample const**)w[6];
    t_sample** realoutputs      = (t_sample **)w[7];
    t_faustgen_tilde *x = (t_faustgen_tilde *)w[8];
    if (!x->f_active) {
      // ag: default `active` flag: bypass or mute the dsp
      if (ninputs == noutputs) {
        for (i = 0; i < ninputs; ++i) {
          for(j = 0; j < nsamples; ++j) {
            realoutputs[i][j] = realinputs[i][j];
          }
        }
      } else {
        for (i = 0; i < noutputs; ++i) {
          for(j = 0; j < nsamples; ++j) {
            realoutputs[i][j] = 0.0;
          }
        }
      }
      return (w+9);
    }
    for(i = 0; i < ninputs; ++i)
    {
        for(j = 0; j < nsamples; ++j)
        {
            faustsigs[i][j] = (FAUSTFLOAT)realinputs[i][j];
        }
    }
    computeCDSPInstance(dsp, nsamples, (FAUSTFLOAT**)faustsigs, (FAUSTFLOAT**)(faustsigs+ninputs));
    for(i = 0; i < noutputs; ++i)
    {
        for(j = 0; j < nsamples; ++j)
        {
            realoutputs[i][j] = (t_sample)faustsigs[ninputs+i][j];
        }
    }
    if (x->f_dsps) {
      // sum up the outputs from all dsp instances
      for (int k = 1; k < x->f_npoly; k++) {
        computeCDSPInstance(x->f_dsps[k], nsamples, (FAUSTFLOAT**)faustsigs, (FAUSTFLOAT**)(faustsigs+ninputs));
        for(i = 0; i < noutputs; ++i)
        {
          for(j = 0; j < nsamples; ++j)
          {
            realoutputs[i][j] += (t_sample)faustsigs[ninputs+i][j];
          }
        }
      }
    }
    // XXXFIXME: If we have multiple dsps (in old-style polyphony), we only
    // output MIDI and OSC data from the first instance here, to prevent
    // duplicate messages. Maybe they should be aggregated instead. (See the
    // remarks in faustgen_tilde_anything.)
    if (x->f_midiout || x->f_midirecv) {
      t_outlet *out = x->f_midiout?faust_io_manager_get_extra_output(x->f_io_manager):NULL;
      faust_ui_manager_midiout(x->f_ui_manager, x->f_midichan, x->f_midirecv, out);
    }
    if (clock_getsystime() >= x->f_next_tick) {
      if (x->f_oscout || x->f_oscrecv) {
        t_outlet *out = x->f_oscout?faust_io_manager_get_extra_output(x->f_io_manager):NULL;
        faust_ui_manager_oscout(x->f_ui_manager, x->f_oscrecv, out);
      }
      if (x->f_instance_name && x->f_instance_name->s_thing)
        faust_ui_manager_gui_update(x->f_ui_manager);
      x->f_next_tick = clock_getsystimeafter(gui_update_time);
    }
    return (w+9);
}

static void faustgen_tilde_free_signals(t_faustgen_tilde *x)
{
    if(x->f_signal_aligned_single)
    {
        free(x->f_signal_aligned_single);
    }
    x->f_signal_aligned_single = NULL;
    if(x->f_signal_matrix_single)
    {
        free(x->f_signal_matrix_single);
    }
    x->f_signal_matrix_single = NULL;
    
    if(x->f_signal_aligned_double)
    {
        free(x->f_signal_aligned_double);
    }
    x->f_signal_aligned_double = NULL;
    if(x->f_signal_matrix_double)
    {
        free(x->f_signal_matrix_double);
    }
    x->f_signal_matrix_double = NULL;
}

static void faustgen_tilde_alloc_signals_single(t_faustgen_tilde *x, size_t const ninputs, size_t const noutputs, size_t const nsamples)
{
    size_t i;
    faustgen_tilde_free_signals(x);
    x->f_signal_aligned_single = (float *)malloc((ninputs + noutputs) * nsamples * sizeof(float));
    if(!x->f_signal_aligned_single)
    {
        pd_error(x, "memory allocation failed");
        return;
    }
    x->f_signal_matrix_single = (float **)malloc((ninputs + noutputs) * sizeof(float *));
    if(!x->f_signal_matrix_single)
    {
        pd_error(x, "memory allocation failed");
        return;
    }
    for(i = 0; i < (ninputs + noutputs); ++i)
    {
        x->f_signal_matrix_single[i] = (x->f_signal_aligned_single+(i*nsamples));
    }
}

static void faustgen_tilde_alloc_signals_double(t_faustgen_tilde *x, size_t const ninputs, size_t const noutputs, size_t const nsamples)
{
    size_t i;
    faustgen_tilde_free_signals(x);
    x->f_signal_aligned_double = (double *)malloc((ninputs + noutputs) * nsamples * sizeof(double));
    if(!x->f_signal_aligned_double)
    {
        pd_error(x, "memory allocation failed");
        return;
    }
    x->f_signal_matrix_double = (double **)malloc((ninputs + noutputs) * sizeof(double *));
    if(!x->f_signal_matrix_double)
    {
        pd_error(x, "memory allocation failed");
        return;
    }
    for(i = 0; i < (ninputs + noutputs); ++i)
    {
        x->f_signal_matrix_double[i] = (x->f_signal_aligned_double+(i*nsamples));
    }
}

static void faustgen_tilde_dsp(t_faustgen_tilde *x, t_signal **sp)
{
    if(x->f_dsp_instance)
    {
        char initialized = getSampleRateCDSPInstance(x->f_dsp_instance) != sp[0]->s_sr;
        if(initialized)
        {
          if (x->f_dsps) {
            for (int i = 0; i < x->f_npoly; i++) {
              // not sure whether we really need to save all of the instances,
              // but to be on the safe side...
              faust_ui_manager_save_states(x->f_uis[i]);
              initCDSPInstance(x->f_dsps[i], sp[0]->s_sr);
            }
          } else {
            faust_ui_manager_save_states(x->f_ui_manager);
            initCDSPInstance(x->f_dsp_instance, sp[0]->s_sr);
          }
        }
        if(!faust_io_manager_prepare(x->f_io_manager, sp))
        {
            size_t const ninputs  = faust_io_manager_get_ninputs(x->f_io_manager);
            size_t const noutputs = faust_io_manager_get_noutputs(x->f_io_manager);
            size_t const nsamples = (size_t)sp[0]->s_n;

            if(x->f_isdouble)
            {
                faustgen_tilde_alloc_signals_double(x, ninputs, noutputs, nsamples);
                dsp_add((t_perfroutine)faustgen_tilde_perform_double, 8,
                        (t_int)x->f_dsp_instance, (t_int)nsamples, (t_int)ninputs, (t_int)noutputs,
                        (t_int)x->f_signal_matrix_double,
                        (t_int)faust_io_manager_get_input_signals(x->f_io_manager),
                        (t_int)faust_io_manager_get_output_signals(x->f_io_manager),
                        (t_int)x);
            }
            else
            {
                faustgen_tilde_alloc_signals_single(x, ninputs, noutputs, nsamples);
                dsp_add((t_perfroutine)faustgen_tilde_perform_single, 8,
                        (t_int)x->f_dsp_instance, (t_int)nsamples, (t_int)ninputs, (t_int)noutputs,
                        (t_int)x->f_signal_matrix_single,
                        (t_int)faust_io_manager_get_input_signals(x->f_io_manager),
                        (t_int)faust_io_manager_get_output_signals(x->f_io_manager),
                        (t_int)x);
            }
        }
        if(initialized)
        {
          if (x->f_dsps) {
            for (int i = 0; i < x->f_npoly; i++) {
              faust_ui_manager_restore_states(x->f_uis[i]);
            }
          } else {
            faust_ui_manager_restore_states(x->f_ui_manager);
          }
        }
    }
}

static t_symbol *make_instance_name(t_symbol *dsp_name, t_symbol *instance_name)
{
  char buf[MAXPDSTRING];
  snprintf(buf, MAXPDSTRING, "%s:%s", dsp_name->s_name,
           instance_name->s_name);
  return gensym(buf);
}

static t_symbol *make_unique_name(t_symbol *dsp_name)
{
  // this simply counts up starting from zero until we find a symbol that's
  // not bound yet, so this will hopefully create reproducible results, as
  // long as the relative order of the faustgen2~ objects in the patch
  // doesn't change
  unsigned counter = 0;
  t_symbol *s;
  do {
    char buf[MAXPDSTRING];
    snprintf(buf, MAXPDSTRING, "%s-%u", dsp_name->s_name, counter);
    s = gensym(buf);
    counter++;
  } while (s->s_thing);
  return s;
}

static void faustgen_tilde_free(t_faustgen_tilde *x)
{
    if (x->f_unique_name) {
      pd_unbind(&x->f_obj.ob_pd, gensym("faustgen2~"));
      pd_unbind(&x->f_obj.ob_pd, x->f_dsp_name);
      pd_unbind(&x->f_obj.ob_pd, x->f_unique_name);
      if (x->f_instance_name) {
        if (x->f_instance_name != x->f_dsp_name)
          pd_unbind(&x->f_obj.ob_pd, x->f_instance_name);
        pd_unbind(&x->f_obj.ob_pd,
                  make_instance_name(x->f_dsp_name, x->f_instance_name));
      }
    }
    faustgen_tilde_delete_instance(x);
    faustgen_tilde_delete_factory(x);
    if (x->f_uis) {
      for (int i = 0; i < x->f_npoly; i++)
        faust_ui_manager_free(x->f_uis[i]);
    } else {
      faust_ui_manager_free(x->f_ui_manager);
    }
    faust_io_manager_free(x->f_io_manager);
    faust_opt_manager_free(x->f_opt_manager);
    faustgen_tilde_free_signals(x);
}

static t_symbol *real_dsp_name(t_symbol *s)
{
  // Remove an optional ~ suffix, which is for cosmetic purposes and to be
  // ignored for searching the dsp file.
  const char *name = s->s_name;
  char buf[MAXPDSTRING];
  size_t l = strlen(name);
  const char p = name[l>0 ? l-1 : l];
  if (p == '~') l--;
  if (l >= MAXPDSTRING) l = MAXPDSTRING-1;
  memset(buf, 0, MAXPDSTRING);
  strncpy(buf, name, l);
  return gensym(buf);
}

static void *faustgen_tilde_new(t_symbol* s, int argc, t_atom* argv)
{
    t_faustgen_tilde* x = (t_faustgen_tilde *)pd_new(faustgen_tilde_class);
    if(x)
    {
        char default_file[MAXPDSTRING];
        bool is_loader_obj = strcmp(s->s_name, "faustgen2~") != 0;
        x->f_canvas = canvas_getcurrent();
        sprintf(default_file, "%s/default", class_gethelpdir(faustgen_tilde_class));
        x->f_dsp_factory    = NULL;
        x->f_dsp_instance   = NULL;
        x->f_dsps = NULL; x->f_uis = NULL;
        
        x->f_signal_matrix_single  = NULL;
        x->f_signal_aligned_single = NULL;
        x->f_signal_matrix_double  = NULL;
        x->f_signal_aligned_double = NULL;
        
        x->f_ui_manager     = faust_ui_manager_new((t_object *)x);
        x->f_io_manager     = faust_io_manager_new((t_object *)x, x->f_canvas);
        x->f_opt_manager    = faust_opt_manager_new((t_object *)x, x->f_canvas);
        x->f_dsp_name       = is_loader_obj ? real_dsp_name(s) :
          argc ? atom_getsymbolarg(0, argc, argv) : gensym(default_file);
        x->f_clock          = clock_new(x, (t_method)faustgen_tilde_autocompile_tick);
        x->f_midiin = x->f_midiout = x->f_oscout = false;
        x->f_midichan = -1;
        x->f_midichanmsk = ALL_CHANNELS;
        x->f_midirecv = x->f_oscrecv = NULL;
        x->f_unique_name = x->f_instance_name = NULL;
        x->f_activesym = gensym("active");
        x->f_active = true;
        x->f_isdouble = false;
        x->f_npoly = 0;
        x->f_voices = NULL;
        // parse the remaining creation arguments
        if (argc > 0 && argv) {
          int n_num = 0;
          // We usually begin with second arg here, as the first arg is the
          // dsp name. That is, unless the object is created from the loader,
          // in which case the dsp name is in the symbol s and arg processing
          // commences at the first arg.
          for (argv = !is_loader_obj?(--argc, argv+1):argv; argc > 0;
               argv++, argc--) {
            if (argv->a_type == A_FLOAT) {
              // float value gives (1-based) MIDI channel, 0 means omni,
              // negative means to block that channel
              add_midichan(x, n_num++, argv->a_w.w_float);
            } else if (argv->a_type == A_SYMBOL &&
                     argv->a_w.w_symbol &&
                     // check that it's not a (compiler) option
                     argv->a_w.w_symbol->s_name[0] != '-')  {
              if (strncmp(argv->a_w.w_symbol->s_name, "midiout=",
                          strlen("midiout=")) == 0) {
                // midiout flag; this can be empty (turning on MIDI output),
                // an integer (turning MIDI output off or on, depending on
                // whether the value is zero or not), or a symbol to be used
                // as a receiver for outgoing MIDI messages
                const char *arg = argv->a_w.w_symbol->s_name+strlen("midiout=");
                unsigned num;
                if (!*arg)
                  x->f_midiout = true;
                else if (sscanf(arg, "%u", &num) == 1)
                  x->f_midiout = num != 0;
                else
                  x->f_midirecv = gensym(arg);
              } else if (strncmp(argv->a_w.w_symbol->s_name, "oscout=",
                                 strlen("oscout=")) == 0) {
                // oscout flag; this can be empty (turning on OSC output),
                // an integer (turning OSC output off or on, depending on
                // whether the value is zero or not), or a symbol to be used
                // as a receiver for outgoing OSC messages
                const char *arg = argv->a_w.w_symbol->s_name+strlen("oscout=");
                unsigned num;
                if (!*arg)
                  x->f_oscout = true;
                else if (sscanf(arg, "%u", &num) == 1)
                  x->f_oscout = num != 0;
                else
                  x->f_oscrecv = gensym(arg);
              } else {
                // the instance name is used as an additional identifier of
                // the dsp in the receivers (see below); the plan is to also
                // employ this to identify a subpatch for an auto-generated Pd
                // GUI a la pd-faust in the future
                x->f_instance_name = argv->a_w.w_symbol;
              }
            } else
              break;
          }
        }
        // any remaining creation arguments are for the compiler
        faust_opt_manager_parse_compile_options(x->f_opt_manager, argc, argv);
        faustgen_tilde_compile(x);
        if(!x->f_dsp_instance)
        {
            faustgen_tilde_free(x);
            return NULL;
        }
        // ag: global faustgen2~ receiver
        pd_bind(&x->f_obj.ob_pd, gensym("faustgen2~"));
        // dsp name
        pd_bind(&x->f_obj.ob_pd, x->f_dsp_name);
        // unique name derived from the dsp name
        x->f_unique_name = make_unique_name(x->f_dsp_name);
        pd_bind(&x->f_obj.ob_pd, x->f_unique_name);
        if (x->f_instance_name) {
          // instance name (if different from the above)
          if (x->f_instance_name != x->f_dsp_name)
            pd_bind(&x->f_obj.ob_pd, x->f_instance_name);
          // dsp-name:instance-name
          pd_bind(&x->f_obj.ob_pd,
                  make_instance_name(x->f_dsp_name, x->f_instance_name));
          // create the Pd GUI
          faust_ui_manager_gui(x->f_ui_manager,
                               x->f_unique_name, x->f_instance_name);
          if (x->f_uis) {
            // also install receivers on the other instances
            for (int i = 1; i < x->f_npoly; i++)
              faust_ui_manager_gui2(x->f_uis[i],
                                    x->f_unique_name, x->f_instance_name);
          }
        }
        // ag: kick off GUI updates every gui_update_time msecs (we do this
        // even if the GUI wasn't created yet, in case it may created later)
        x->f_next_tick = clock_getsystimeafter(gui_update_time);
    }
    return x;
}

#ifndef _WIN32
#define __USE_GNU 1 // to get RTLD_DEFAULT
#include <dlfcn.h> // for dlsym
#ifndef RTLD_DEFAULT
/* If RTLD_DEFAULT still isn't defined then just passing NULL will hopefully
   do the trick. */
#define RTLD_DEFAULT NULL
#endif
#endif

// adapted from the pd-lua loader, thanks to Claude and Iohannes!

#ifdef UNUSED
#elif defined(__GNUC__)
# define UNUSED(x) UNUSED_ ## x __attribute__((unused))
#elif defined(__LCLINT__)
# define UNUSED(x) /*@unused@*/ x
#else
# define UNUSED(x) x
#endif

static int faustgen_loader(t_symbol *name)
{
  t_class* c = class_new(name,
                         (t_newmethod)faustgen_tilde_new,
                         (t_method)faustgen_tilde_free,
                         sizeof(t_faustgen_tilde), CLASS_DEFAULT, A_GIMME, 0);
  if (c) {
    class_addmethod(c,  (t_method)faustgen_tilde_dsp,               gensym("dsp"),              A_CANT, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_compile,           gensym("compile"),          A_NULL, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_compile_options,   gensym("compileoptions"),   A_GIMME, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_autocompile,       gensym("autocompile"),      A_GIMME, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_print,             gensym("print"),            A_NULL, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_dump,              gensym("dump"),             A_DEFSYM, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_tuning,            gensym("tuning"),           A_GIMME, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_defaults,          gensym("defaults"),         A_NULL, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_gui,               gensym("gui"),              A_NULL, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_oscout,            gensym("oscout"),           A_GIMME, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_midiout,           gensym("midiout"),          A_GIMME, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_midichan,          gensym("midichan"),         A_GIMME, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_menu_open,         gensym("click"),            A_NULL, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_menu_open,         gensym("menu-open"),        A_NULL, 0);
    class_addbang(c, (t_method)faustgen_tilde_allnotesoff);
    class_addanything(c, (t_method)faustgen_tilde_anything);
  }
  return c != NULL;
}

static int faustgen_loader_pathwise
(
 t_canvas    *UNUSED(canvas), // Pd canvas to use to find the dsp.
 const char  *name, // The name of the dsp (without .dsp extension).
 const char  *path // The directory to search for the dsp.
)
{
  char dirbuf[MAXPDSTRING], *ptr;
  int fd, result = 0;

  if (!path) {
    // we already tried all paths, report failure
    return 0;
  }
  // We allow a ~ to be tacked on to the dsp name, so compute the real name of
  // the dsp file here.
  t_symbol *s = real_dsp_name(gensym(name));
  fd = sys_trytoopenone(path, s->s_name, ".dsp",
                        dirbuf, &ptr, MAXPDSTRING, 1);
  if (fd >= 0) {
    // the actual loading, compiling etc. is done in the class setup
    sys_close(fd);
    result = faustgen_loader(gensym(name));
  }
  return result;
}

static int faustgen_loader_legacy(t_canvas *canvas, char *name)
{
  char dirbuf[MAXPDSTRING], *ptr;
  int fd, result = 0;

  t_symbol *s = real_dsp_name(gensym(name));
  fd = canvas_open(canvas, s->s_name, ".dsp", dirbuf, &ptr, MAXPDSTRING, 1);
  if (fd>=0) {
    sys_close(fd);
    result = faustgen_loader(gensym(name));
  }
  return result;
}

void faustgen2_tilde_setup(void)
{
  // register the faustgen2~ loader
  int major = 0, minor = 0, patchlevel = 0;
  sys_getversion(&major, &minor, &patchlevel);
  if (major ==0 && minor < 47)
    // before Pd<0.47, the loaders had to iterate over each path themselves
    sys_register_loader((loader_t)faustgen_loader_legacy);
  else
    // since Pd>=0.47, Pd tries the loaders for each path
    sys_register_loader((loader_t)faustgen_loader_pathwise);
  // register the faustgen2~ class
  t_class* c = class_new(gensym("faustgen2~"),
                         (t_newmethod)faustgen_tilde_new,
                         (t_method)faustgen_tilde_free,
                         sizeof(t_faustgen_tilde), CLASS_DEFAULT, A_GIMME, 0);
    
  if (c) {
    class_addmethod(c,  (t_method)faustgen_tilde_dsp,               gensym("dsp"),              A_CANT, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_compile,           gensym("compile"),          A_NULL, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_compile_options,   gensym("compileoptions"),   A_GIMME, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_autocompile,       gensym("autocompile"),      A_GIMME, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_print,             gensym("print"),            A_NULL, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_dump,              gensym("dump"),             A_DEFSYM, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_tuning,            gensym("tuning"),           A_GIMME, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_defaults,          gensym("defaults"),         A_NULL, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_gui,               gensym("gui"),              A_NULL, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_oscout,            gensym("oscout"),           A_GIMME, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_midiout,           gensym("midiout"),          A_GIMME, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_midichan,          gensym("midichan"),         A_GIMME, 0);
#if 0
    class_addmethod(c,  (t_method)faustgen_tilde_open_texteditor,   gensym("click"),            A_NULL, 0);
#endif
    class_addmethod(c,  (t_method)faustgen_tilde_menu_open,         gensym("click"),            A_NULL, 0);
    class_addmethod(c,  (t_method)faustgen_tilde_menu_open,         gensym("menu-open"),        A_NULL, 0);
        
    //class_addmethod(c,      (t_method)faustgen_tilde_read,             gensym("read"),           A_SYMBOL);
    class_addbang(c, (t_method)faustgen_tilde_allnotesoff);
    class_addanything(c, (t_method)faustgen_tilde_anything);
    post("faustgen2~ version: %s, https://github.com/agraef/pd-faustgen", FAUSTGEN_VERSION_STR);
    post("Copyright (c) 2018 Pierre Guillot, (c) 2020 Albert Gräf");
    post("Faust version: %s, https://faust.grame.fr", getCLibFaustVersion());
    post("Copyright (c) 2002-2022 GRAME et al");
    logpost(NULL, 3, "faustgen2~ default include directory: %s", class_gethelpdir(c));

  }
    
  faustgen_tilde_class = c;
#ifdef _WIN32
  nw_gui_vmess = (void*)GetProcAddress(GetModuleHandleA("pd.dll"), "gui_vmess");
#else
  nw_gui_vmess = dlsym(RTLD_DEFAULT, "gui_vmess");
#endif
  if (nw_gui_vmess) post("faustgen2~: using JavaScript interface (nw.js)");
  faust_ui_receive_setup();
}

