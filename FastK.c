 /********************************************************************************************
 *
 *  FastK: a rapid disk-based k-mer counter for high-fidelity shotgun data sets.
 *     Uses a novel minimizer-based distribution scheme that permits problems of
 *     arbitrary size, and a two-staged "super-mer then weighted k-mer" sort to acheive
 *     greater speed when error rates are low (1% or less).  Directly produces sequence
 *     profiles.
 *
 *  Author:  Gene Myers
 *  Date  :  October, 2020
 *
 *********************************************************************************************/
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/resource.h>
#include <math.h>
#include <time.h>

#include "gene_core.h"
#include "FastK.h"

#ifdef DEVELOPER

int    DO_STAGE;   //  Which step to perform

#endif

static char *Usage[] = { "[-k<int(40)>] -t[<int(4)>]] [-p[:<table>[.ktab]]] [-c] [-bc<int(0)>]",
                         "  [-v] [-N<path_name>] [-P<dir(/tmp)>] [-M<int(12)>] [-T<int(4)>]",
                         "    <source>[.cram|.[bs]am|.db|.dam|.f[ast][aq][.gz] ..."
                       };

  //  Option Settings

int    VERBOSE;      //  show progress
int    NTHREADS;     //  # of threads to run with
int      ITHREADS;     //  # of threads possible for input
int64  SORT_MEMORY;  //  GB of memory for downstream KMcount sorts
char  *SORT_PATH;    //  where to put external files

int    KMER;         //  desired K-mer length
int    DO_TABLE;     // Zero or table cutoff
int    DO_PROFILE;   // Do or not
int      PRO_THREADS;  //  If > 0, # of threads in .ktab for profile
char    *PRO_HIDDEN;   //  Root of .ktab hidden files for profile
int    BC_PREFIX;    // Ignore prefix of each sequence of this length
char  *OUT_NAME;     // Prefix root for all output file names
int    COMPRESS;     // Homopoloymer compress input

  //  Major parameters, sizes of things

int    NPARTS;       //  # of k-mer buckets to use
int    SMER;         //  size of a super-mer for sorts
int64  KMAX;         //  max k-mers in any part
int64  NMAX;         //  max super-mers in any part
int64  RMAX;         //  max run index for any thread

int    MOD_LEN;     //  length of minimizer buffer (power of 2)
int    MOD_MSK;     //  mask for minimzer buffer

int    MAX_SUPER;    //  = (KMER - PAD_LEN) + 1

int    SLEN_BITS;    //  # of bits needed to encode the length of a super-mer (less KMER-1)
uint64 SLEN_BIT_MASK;   //  Bit-mask for super-mer lengths

int RUN_BITS;     //  # of bits encoding largest run index
int RUN_BYTES;    //  # of bytes encoding largest run index

int SLEN_BYTES;   //  # of bytes encoding length of a super-mer
int PLEN_BYTES;   //  # of bytes encoding length of a compressed profile segment
int PROF_BYTES;   //  # of bytes encoding RUN_BYTES+PLEN_BYTES

int SLEN_BYTE_MASK;   //  Byte-mask for super-mer lengths

int KMER_BYTES;   //  # of bytes encoding a KMER
int SMER_BYTES;   //  # of bytes encoding a super-mer
int KMAX_BYTES;   //  # of bytes to encode an int in [0,KMAX)

int KMER_WORD;    //  bytes to hold a k-mer entry
int SMER_WORD;    //  bytes to hold a super-mer entry
int TMER_WORD;    //  bytes to hold a k-mer table entry
int CMER_WORD;    //  bytes to hold a count/index entry

static struct rusage   Itime;
static struct timespec Iwall;

void startTime()
{ getrusage(RUSAGE_SELF,&Itime);
  clock_gettime(CLOCK_MONOTONIC,&Iwall);
}

void timeTo(FILE *f)
{ struct rusage   now;
  struct timespec today;
  int usecs, umics;
  int ssecs, smics;
  int tsecs, tmics;
  int64 mem;

  getrusage(RUSAGE_SELF, &now);
  clock_gettime(CLOCK_MONOTONIC,&today);

  fprintf (f,"\nResources:");

  usecs = now.ru_utime.tv_sec  - Itime.ru_utime.tv_sec;
  umics = now.ru_utime.tv_usec - Itime.ru_utime.tv_usec;
  if (umics < 0)
    { umics += 1000000;
      usecs -= 1;
    }
  if (usecs >= 60)
    fprintf (f,"  %d:%02d.%03du",usecs/60,usecs%60,umics/1000);
  else
    fprintf (f,"  %d.%03du",usecs,umics/1000);

  ssecs = now.ru_stime.tv_sec  - Itime.ru_stime.tv_sec;
  smics = now.ru_stime.tv_usec - Itime.ru_stime.tv_usec;
  if (smics < 0)
    { smics += 1000000;
      ssecs -= 1;
    }
  if (ssecs >= 60)
    fprintf (f,"  %d:%02d.%03ds",ssecs/60,ssecs%60,smics/1000);
  else
    fprintf (f,"  %d.%03ds",ssecs,smics/1000);

  tsecs = today.tv_sec  - Iwall.tv_sec;
  tmics = today.tv_nsec/1000 - Iwall.tv_nsec/1000;
  if (tmics < 0)
    { tmics += 1000000;
      tsecs -= 1;
    }
  if (tsecs >= 60)
    fprintf (f,"  %d:%02d.%03dw",tsecs/60,tsecs%60,tmics/1000);
  else
    fprintf (f,"  %d.%03dw",tsecs,tmics/1000);

  fprintf(f,"  %.1f%%  ",(100.*(usecs+ssecs) + (umics+smics)/10000.)/(tsecs+tmics/1000000.));

  mem = (now.ru_maxrss - Itime.ru_maxrss)/1000000;
  Print_Number(mem,0,f);
  fprintf(f,"MB");

  fprintf(f,"\n");
}


int main(int argc, char *argv[])
{ char  *root;
  char  *pwd;

  startTime();

  { int    i, j, k;
    int    flags[128];
    int    memory, promer; 
    char  *eptr;

    ARG_INIT("FastK")

    KMER        = 40;
    SORT_MEMORY = 12000000000ll;
    NTHREADS    = 4;
    SORT_PATH   = "/tmp";
    DO_TABLE    = 0;
    DO_PROFILE  = 0;
    PRO_THREADS = 0;
    BC_PREFIX   = 0;
    OUT_NAME    = NULL;
#ifdef DEVELOPER
    DO_STAGE    = 0;
#endif

    j = 1;
    for (i = 1; i < argc; i++)
      if (argv[i][0] == '-')
        switch (argv[i][1])
        { default:
            ARG_FLAGS("vcpt")
            break;
          case 'b':
            if (argv[i][2] != 'c')
              { fprintf(stderr,"\n%s: -%s is not a legal optional argument\n",Prog_Name,argv[i]);
                exit (1);
              }                
            argv[i] += 1;
            ARG_NON_NEGATIVE(BC_PREFIX,"Bar code prefiex")
            argv[i] -= 1;
            break;
          case 'k':
            ARG_POSITIVE(KMER,"K-mer length")
            break;
          case 'p':
            if (argv[i][2] != ':')
              { ARG_FLAGS("vcpt");
                break;
              }
            { char *d, *r;
              FILE *f;

              d = PathTo(argv[i]+3);
              r = Root(argv[i]+3,".ktab");
              PRO_HIDDEN = Strdup(Catenate(d,"/.",r,""),NULL);
              f = fopen(Catenate(d,"/",r,".ktab"),"r");
              if (f == NULL)
                { fprintf(stderr,"%s: Cannot find stub file %s.ktab\n",Prog_Name,r);
                  exit (1);
                }
              free(r);
              free(d);
              fread(&promer,sizeof(int),1,f);
              fread(&PRO_THREADS,sizeof(int),1,f);
              if (PRO_THREADS <= 0)
                { fprintf(stderr,"%s: %s.ktab has no hidden files?\n",Prog_Name,r);
                  exit (1);
                }
              fclose(f);
              DO_PROFILE = 1;
            }
            break;
          case 't':
            if (argv[i][2] == '\0' || isalpha(argv[i][2]))
              { ARG_FLAGS("vcpt");
                break;
              }
            ARG_POSITIVE(DO_TABLE,"Cutoff for k-mer table")
            break;
          case 'M':
            ARG_POSITIVE(memory,"GB of memory for sorting step")
            SORT_MEMORY = memory * 1000000000ll;
            break;
          case 'N':
            OUT_NAME = argv[i]+2;
            break;
          case 'P':
            SORT_PATH = argv[i]+2;
            break;
          case 'T':
            ARG_POSITIVE(NTHREADS,"Number of threads")
            break;
#ifdef DEVELOPER
          case '1':
            DO_STAGE = 1;
            break;
          case '2':
            DO_STAGE = 2;
            break;
          case '3':
            DO_STAGE = 3;
            break;
          case '4':
            DO_STAGE = 4;
            break;
#endif
        }
      else
        argv[j++] = argv[i];
    argc = j;

    VERBOSE    = flags['v'];   //  Globally declared in filter.h
    COMPRESS   = flags['c'];
    if (flags['t'])
      DO_TABLE = 4;
    if (flags['p'])
      DO_PROFILE = 1;

    if (PRO_THREADS > 0)
      { if (promer != KMER)
          { fprintf(stderr,"%s: -p table k-mer size (%d) != k-mer specified (%d)\n",
                           Prog_Name,promer,KMER);
            exit (1);
          }
        fprintf(stderr,"%s: Sorry -p:ktab feature not yet functional\n",Prog_Name);
        exit (1);
      }

    if (argc < 2)
      { fprintf(stderr,"\nUsage: %s %s\n",Prog_Name,Usage[0]);
        fprintf(stderr,"       %*s %s\n",(int) strlen(Prog_Name),"",Usage[1]);
        fprintf(stderr,"       %*s %s\n",(int) strlen(Prog_Name),"",Usage[2]);
        fprintf(stderr,"\n");
        fprintf(stderr,"      -v: Verbose mode, output statistics as proceed.\n");
        fprintf(stderr,"      -T: Use -T threads.\n");
        fprintf(stderr,"      -N: Use given path for output directory and root name prefix.\n");
        fprintf(stderr,"      -P: Place block level sorts in directory -P.\n");
        fprintf(stderr,"      -M: Use -M GB of memory in downstream sorting steps of KMcount.\n");
        fprintf(stderr,"\n");
        fprintf(stderr,"      -k: k-mer size.\n");
        fprintf(stderr,"      -t: Produce table of sorted k-mer & counts >= level specified\n");
        fprintf(stderr,"      -p: Produce sequence count profiles (w.r.t. table if given)\n");
        fprintf(stderr,"     -bc: Ignore prefix of each read of given length (e.g. bar code)\n");
        fprintf(stderr,"      -c: Homopolymer compress every sequence\n");
        exit (1);
      }
  }

  //  Get full path strong for sorting subdirectory (in variable SORT_PATH)

  { char  *cpath, *spath;
    DIR   *dirp;

    if (SORT_PATH[0] != '/')
      { cpath = getcwd(NULL,0);
        if (SORT_PATH[0] == '.')
          { if (SORT_PATH[1] == '/')
              spath = Catenate(cpath,SORT_PATH+1,"","");
            else if (SORT_PATH[1] == '\0')
              spath = cpath;
            else
              { fprintf(stderr,"\n%s: -P option: . not followed by /\n",Prog_Name);
                exit (1);
              }
          }
        else
          spath = Catenate(cpath,"/",SORT_PATH,"");
        SORT_PATH = Strdup(spath,"Allocating path");
        free(cpath);
      }
    else
      SORT_PATH = Strdup(SORT_PATH,"Allocating path");

    if ((dirp = opendir(SORT_PATH)) == NULL)
      { fprintf(stderr,"\n%s: -P option: cannot open directory %s\n",Prog_Name,SORT_PATH);
        exit (1);
      }
    closedir(dirp);
  }

  { Input_Partition *io;
    DATA_BLOCK      *block;
    int64            gsize;
    int              rsize, val;

    io = Partition_Input(argc,argv);

    if (OUT_NAME == NULL)
      { root = First_Root(io);
        pwd  = First_Pwd (io);
      }
    else
      { root = Root(OUT_NAME,NULL);
        pwd  = PathTo(OUT_NAME);
      }

    if (VERBOSE)
      fprintf(stderr,"\nDetermining minimizer scheme & partition for %s\n",root);

    //  Determine number of buckets and padded minimzer scheme based on first
    //    block of the data set

    block = Get_First_Block(io,1000000000);

    KMER_BYTES = (KMER*2+7) >> 3;

    rsize  = KMER_BYTES + 2;
    gsize  = block->totlen - KMER*block->nreads;
    if (gsize < block->totlen/3)
      { fprintf(stderr,"\n%s: Sequences are on average smaller than 1.5x k-mer size!\n",Prog_Name);
        exit (1);
      }
    gsize  = gsize*block->ratio*rsize;
    NPARTS = (gsize-1)/SORT_MEMORY + 1;

    if (VERBOSE)
      { double est = gsize/(1.*rsize);
        if (est >= 5.e8)
          fprintf(stderr,"  Estimate %.3fG",est/1.e9);
        else if (est >= 5.e5)
          fprintf(stderr,"  Estimate %.3fM",est/1.e6);
        else
          fprintf(stderr,"  Estimate %.3fK",est/1.e3);
        fprintf(stderr," %d-%smers\n",KMER,COMPRESS?"hoco-":"");
        if (NPARTS > 1)
          fprintf(stderr,"  Dividing data into %d buckets\n",NPARTS);
        else
          fprintf(stderr,"  Handling data in a single bucket\n");
      }

    MOD_LEN = 1;
    while (MOD_LEN < KMER)
      MOD_LEN <<= 1;
    MOD_LEN <<= 1;
    MOD_MSK = MOD_LEN-1;

    MAX_SUPER = Determine_Scheme(block);

    Free_First_Block(block);

    SMER = MAX_SUPER + KMER - 1;
  
    SLEN_BITS = 0;
    for (val = MAX_SUPER; val > 0; val >>= 1)
      SLEN_BITS += 1;
    SLEN_BIT_MASK = (0x1u << SLEN_BITS)-1;
    SLEN_BYTES    = (SLEN_BITS+7) >> 3;

    SMER_BYTES = (SMER*2+7) >> 3;
    SMER_WORD  = SMER_BYTES + SLEN_BYTES;
    KMER_WORD  = KMER_BYTES + 2;
    PLEN_BYTES = (SLEN_BITS+8) >> 3;
    TMER_WORD  = KMER_BYTES + 2;

    //  Make sure you can open (NPARTS + 2) * NTHREADS + tid files and then set up data structures
    //    for each such file.  tid is typically 3 unless using valgrind or other instrumentation.

    { struct rlimit rlp;
      int           tid;
      uint64        nfiles;

      tid = open(".xxx",O_CREAT|O_TRUNC|O_WRONLY,S_IRWXU);
      close(tid);
      unlink(".xxx");

      nfiles = (NPARTS+2)*NTHREADS + tid;
      getrlimit(RLIMIT_NOFILE,&rlp);
      if (nfiles > rlp.rlim_max)
        { fprintf(stderr,"\n%s: Cannot open %lld files simultaneously\n",Prog_Name,nfiles);
          exit (1);
        }
      rlp.rlim_cur = nfiles;
      setrlimit(RLIMIT_NOFILE,&rlp);
    }

#ifdef DEVELOPER
    if (DO_STAGE == 1)
#endif
      Split_Kmers(io,root);

    Free_Input_Partition(io);
  }

#ifdef DEVELOPER
  if (DO_STAGE == 2)
#endif
    Sorting(pwd,root);

  if (DO_TABLE > 0)
#ifdef DEVELOPER
    if (DO_STAGE == 3)
      Merge_Tables(pwd,root);
#else
    Merge_Tables(pwd,root);
#endif

  if (DO_PROFILE > 0)
#ifdef DEVELOPER
    if (DO_STAGE == 4)
      Merge_Profiles(pwd,root);
#else
    Merge_Profiles(pwd,root);
#endif

#ifndef DEVELOPER
  free(NUM_RID);
#endif

  free(pwd);
  free(root);
  free(SORT_PATH);

  if (PRO_THREADS > 0)
    free(PRO_HIDDEN);

  Catenate(NULL,NULL,NULL,NULL);  //  frees internal buffers of these routines
  Numbered_Suffix(NULL,0,NULL);
  free(Prog_Name);

  if (VERBOSE)
    timeTo(stderr);

  exit (0);
}
