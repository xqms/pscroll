/************************************************************
Copyright (c) 1993 by Silicon Graphics Computer Systems, Inc.

Permission to use, copy, modify, and distribute this
software and its documentation for any purpose and without
fee is hereby granted, provided that the above copyright
notice appear in all copies and that both that copyright
notice and this permission notice appear in supporting
documentation, and that the name of Silicon Graphics not be 
used in advertising or publicity pertaining to distribution 
of the software without specific prior written permission.
Silicon Graphics makes no representation about the suitability 
of this software for any purpose. It is provided "as is"
without any express or implied warranty.

SILICON GRAPHICS DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS 
SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY 
AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL SILICON
GRAPHICS BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL 
DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, 
DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE 
OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION  WITH
THE USE OR PERFORMANCE OF THIS SOFTWARE.

********************************************************/

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#ifdef HAVE_XKB_CONFIG_H
#include <xkb-config.h>
#endif

#ifdef HAVE_SHA1_IN_LIBGCRYPT /* Use libgcrypt for SHA1 */
# include <gcrypt.h>
#else /* Use OpenSSL's libcrypto */
#warning "xkbcomp caching support disabled"
#endif

#include <stdio.h>
#include <ctype.h>
#include <X11/X.h>
#include <X11/Xos.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/extensions/XKM.h>
#include "inputstr.h"
#include "scrnintstr.h"
#include "windowstr.h"
#define	XKBSRV_NEED_FILE_FUNCS
#include <xkbsrv.h>
#include <X11/extensions/XI.h>
#include <errno.h>
#include "xkb.h"

#if defined(CSRG_BASED) || defined(linux) || defined(__GNU__)
#include <paths.h>
#endif

#define	PRE_ERROR_MSG "\"The XKEYBOARD keymap compiler (xkbcomp) reports:\""
#define	ERROR_PREFIX	"\"> \""
#define	POST_ERROR_MSG1 "\"Errors from xkbcomp are not fatal to the X server\""
#define	POST_ERROR_MSG2 "\"End of messages from xkbcomp\""

#if defined(WIN32)
#define PATHSEPARATOR "\\"
#else
#define PATHSEPARATOR "/"
#endif

#ifdef WIN32

#include <X11/Xwindows.h>
const char* 
Win32TempDir()
{
    static char buffer[PATH_MAX];
    if (GetTempPath(sizeof(buffer), buffer))
    {
        int len;
        buffer[sizeof(buffer)-1] = 0;
        len = strlen(buffer);
        if (len > 0)
            if (buffer[len-1] == '\\')
                buffer[len-1] = 0;
        return buffer;
    }
    if (getenv("TEMP") != NULL)
        return getenv("TEMP");
    else if (getenv("TMP") != NULL)
        return getenv("TEMP");
    else
        return "/tmp";
}

int 
Win32System(const char *cmdline)
{
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    DWORD dwExitCode;
    char *cmd = xstrdup(cmdline);

    ZeroMemory( &si, sizeof(si) );
    si.cb = sizeof(si);
    ZeroMemory( &pi, sizeof(pi) );

    if (!CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) 
    {
	LPVOID buffer;
	if (!FormatMessage( 
		    FORMAT_MESSAGE_ALLOCATE_BUFFER | 
		    FORMAT_MESSAGE_FROM_SYSTEM | 
		    FORMAT_MESSAGE_IGNORE_INSERTS,
		    NULL,
		    GetLastError(),
		    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		    (LPTSTR) &buffer,
		    0,
		    NULL ))
	{
	    ErrorF("[xkb] Starting '%s' failed!\n", cmdline); 
	}
	else
	{
	    ErrorF("[xkb] Starting '%s' failed: %s", cmdline, (char *)buffer); 
	    LocalFree(buffer);
	}

	xfree(cmd);
	return -1;
    }
    /* Wait until child process exits. */
    WaitForSingleObject( pi.hProcess, INFINITE );

    GetExitCodeProcess( pi.hProcess, &dwExitCode);
    
    /* Close process and thread handles. */
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    xfree(cmd);

    return dwExitCode;
}
#undef System
#define System(x) Win32System(x)
#endif

static void
OutputDirectory(
    char* outdir,
    size_t size)
{
#ifndef WIN32
    /* Can we write an xkm and then open it too? */
    if (access(XKM_OUTPUT_DIR, W_OK | X_OK) == 0 && (strlen(XKM_OUTPUT_DIR) < size))
    {
	(void) strcpy (outdir, XKM_OUTPUT_DIR);
    } else
#else
    if (strlen(Win32TempDir()) + 1 < size)
    {
	(void) strcpy(outdir, Win32TempDir());
	(void) strcat(outdir, "\\");
    } else 
#endif
    if (strlen("/tmp/") < size)
    {
	(void) strcpy (outdir, "/tmp/");
    }
}

#ifndef SHA_DIGEST_LENGTH
#define SHA_DIGEST_LENGTH 20
#endif

static Bool
Sha1Asc(char sha1Asc[SHA_DIGEST_LENGTH*2+1], const char * input)
{
    int i;
    unsigned char sha1[SHA_DIGEST_LENGTH];

#ifdef HAVE_SHA1_IN_LIBGCRYPT /* Use libgcrypt for SHA1 */
    static int init;
    gcry_md_hd_t h;
    gcry_error_t err;

    if (!init) {
	if (!gcry_check_version(NULL))
	    return BadAlloc;
	gcry_control(GCRYCTL_DISABLE_SECMEM, 0);
	gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
	init = 1;
    }

    err = gcry_md_open(&h, GCRY_MD_SHA1, 0);
    if (err)
	return BadAlloc;
    gcry_md_write(h, input, strlen(input));
    memcpy(sha1, gcry_md_read(h, GCRY_MD_SHA1), 20);
    gcry_md_close(h);
#endif

    /* convert sha1 to sha1_asc */
    for(i=0; i<SHA_DIGEST_LENGTH; ++i) {
        sprintf(sha1Asc+i*2, "%02X", sha1[i]);
    }

    return Success;
}

/* call xkbcomp and compile XKB keymap, return xkm file name in
   nameRtrn */
static Bool
XkbDDXCompileKeymapByNames(	XkbDescPtr		xkb,
				XkbComponentNamesPtr	names,
				unsigned		want,
				unsigned		need,
				char *			nameRtrn,
				int			nameRtrnLen)
{
    FILE *	out;
    char *	buf = NULL, xkmfile[PATH_MAX], xkm_output_dir[PATH_MAX];
    char *	tmpXkmFile = NULL;
    char *	canonicalXkmFileName = NULL;
    char	sha1Asc[SHA_DIGEST_LENGTH*2+1], xkbKeyMapBuf[100*1024];
    int	ret, result;

    const char	*emptystring = "";
    const char	*xkbbasedirflag = emptystring;
    const char	*xkbbindir = emptystring;
    const char	*xkbbindirsep = emptystring;

#ifdef WIN32
    /* WIN32 has no popen. The input must be stored in a file which is
       used as input for xkbcomp. xkbcomp does not read from stdin. */
    char tmpname[PATH_MAX];
    const char *xkbfile = tmpname;
#else
    const char *xkbfile = "-";
#endif

    /* Write keymap source (xkbfile) to memory buffer `xkbKeyMapBuf',
       of which SHA1 is generated and used as result xkm file name  */
    memset(xkbKeyMapBuf, 0, sizeof(xkbKeyMapBuf));
    out = fmemopen(xkbKeyMapBuf, sizeof(xkbKeyMapBuf), "w");
    if (NULL == out) {
        ErrorF("[xkb] Open xkbKeyMapBuf for writing failed\n");
        return False;
    }
    ret = XkbWriteXKBKeymapForNames(out, names, xkb, want, need);
    if (fclose(out) !=0)
    {
        ErrorF("[xkb] XkbWriteXKBKeymapForNames error, perhaps xkbKeyMapBuf is too small\n");
        return False;
    }
#ifdef DEBUG
    if (xkbDebugFlags) {
       ErrorF("[xkb] XkbDDXCompileKeymapByNames compiling keymap:\n");
       fputs(xkbKeyMapBuf, stderr);
    }
#endif
    if (!ret) {
        ErrorF("[xkb] Generating XKB Keymap failed, giving up compiling keymap\n");
        return False;
    }

    DebugF("[xkb] computing SHA1 of keymap\n");
    if (Success == Sha1Asc(sha1Asc, xkbKeyMapBuf)) {
        snprintf(xkmfile, sizeof(xkmfile), "server-%s", sha1Asc);
    }
    else {
        ErrorF("[xkb] Computing SHA1 of keymap failed, "
               "using display name instead as xkm file name\n");
        snprintf(xkmfile, sizeof(xkmfile), "server-%s", display);
    }

    OutputDirectory(xkm_output_dir, sizeof(xkm_output_dir));
    /* set nameRtrn, fail if it's too small */
    if ((strlen(xkmfile)+1 > nameRtrnLen) && nameRtrn) {
        ErrorF("[xkb] nameRtrn too small to hold xkmfile name\n");
        return False;
    }
    strncpy(nameRtrn, xkmfile, nameRtrnLen);

    /* if the xkm file already exists, reuse it */
    canonicalXkmFileName = Xprintf("%s%s.xkm", xkm_output_dir, xkmfile);
    if (access(canonicalXkmFileName, R_OK) == 0) {
        /* yes, we can reuse the old xkm file */
        LogMessage(X_INFO, "XKB: reuse xkmfile %s\n", canonicalXkmFileName);
        result = True;
        goto _ret;
    }
    LogMessage(X_INFO, "XKB: generating xkmfile %s\n", canonicalXkmFileName);

    /* continue to call xkbcomp to compile the keymap. to avoid race
       condition, we compile it to a tmpfile then rename it to
       xkmfile */

#ifdef WIN32
    strcpy(tmpname, Win32TempDir());
    strcat(tmpname, "\\xkb_XXXXXX");
    (void) mktemp(tmpname);
#endif

    if (XkbBaseDirectory != NULL) {
	xkbbasedirflag = Xprintf("\"-R%s\"", XkbBaseDirectory);
    }

    if (XkbBinDirectory != NULL) {
	int ld = strlen(XkbBinDirectory);
	int lps = strlen(PATHSEPARATOR);

	xkbbindir = XkbBinDirectory;

	if ((ld >= lps) &&
	    (strcmp(xkbbindir + ld - lps, PATHSEPARATOR) != 0)) {
	    xkbbindirsep = PATHSEPARATOR;
	}
    }

    if ( (tmpXkmFile = tempnam(xkm_output_dir, NULL)) == NULL ) {
        ErrorF("[xkb] Can't generate temp xkm file name");
        result = False;
        goto _ret;
    }

    buf = Xprintf("\"%s%sxkbcomp\" -w %d %s -xkm \"%s\" "
		  "-em1 %s -emp %s -eml %s \"%s\"",
		  xkbbindir, xkbbindirsep,
		  ( (xkbDebugFlags < 2) ? 1 :
		    ((xkbDebugFlags > 10) ? 10 : (int)xkbDebugFlags) ),
		  xkbbasedirflag, xkbfile,
		  PRE_ERROR_MSG, ERROR_PREFIX, POST_ERROR_MSG1,
		  tmpXkmFile);

    if (xkbbasedirflag != emptystring) {
	xfree(xkbbasedirflag);
    }
    
    /* there's a potential race condition between calling tempnam()
       and invoking xkbcomp to write the result file (potential temp
       file name conflicts), but since xkbcomp is a standalone
       program, we have to live with this */

#ifndef WIN32
    out= Popen(buf,"w");
#else
    out= fopen(tmpname, "w");
#endif
    
    if (out!=NULL) {
        /* write XKBKeyMapBuf to xkbcomp */
        if (EOF==fputs(xkbKeyMapBuf, out))
        {
            ErrorF("[xkb] Sending keymap to xkbcomp failed\n");
            result = False;
            goto _ret;
    }
#ifndef WIN32
	if (Pclose(out)==0)
#else
	if (fclose(out)==0 && System(buf) >= 0)
#endif
	{
        /* xkbcomp success */
            if (xkbDebugFlags)
                DebugF("[xkb] xkb executes: %s\n",buf);

            /* if canonicalXkmFileName already exists now, we simply
               overwrite it, this is OK */
            ret = rename(tmpXkmFile, canonicalXkmFileName);
            if (0 != ret) {
                ErrorF("[xkb] Can't rename %s to %s, error: %s\n",
                       tmpXkmFile, canonicalXkmFileName,
                       strerror(errno));

                /* in case of error, don't unlink tmpXkmFile, leave it
                   for debugging */

                result = False;
                goto _ret;
	    }

            result = True;
            goto _ret;
	}
	else
	    LogMessage(X_ERROR, "Error compiling keymap (%s)\n", xkbfile);
#ifdef WIN32
        /* remove the temporary file */
        unlink(tmpname);
#endif
    }
    else {
#ifndef WIN32
	LogMessage(X_ERROR, "XKB: Could not invoke xkbcomp\n");
#else
	LogMessage(X_ERROR, "Could not open file %s\n", tmpname);
#endif
    }
    if (nameRtrn)
	nameRtrn[0]= '\0';
    result = False;

_ret:
    if (tmpXkmFile)
        free(tmpXkmFile);
    if (canonicalXkmFileName)
        xfree(canonicalXkmFileName);
    if (buf != NULL)
        xfree (buf);

    return result;
}

static FILE *
XkbDDXOpenConfigFile(char *mapName,char *fileNameRtrn,int fileNameRtrnLen)
{
char	buf[PATH_MAX],xkm_output_dir[PATH_MAX];
FILE *	file;

    buf[0]= '\0';
    if (mapName!=NULL) {
	OutputDirectory(xkm_output_dir, sizeof(xkm_output_dir));
	if ((XkbBaseDirectory!=NULL)&&(xkm_output_dir[0]!='/')
#ifdef WIN32
                &&(!isalpha(xkm_output_dir[0]) || xkm_output_dir[1]!=':')
#endif
                ) {
	    if (strlen(XkbBaseDirectory)+strlen(xkm_output_dir)
		     +strlen(mapName)+6 <= PATH_MAX)
	    {
	        sprintf(buf,"%s/%s%s.xkm",XkbBaseDirectory,
					xkm_output_dir,mapName);
	    }
	}
	else if (strlen(xkm_output_dir)+strlen(mapName)+5 <= PATH_MAX)
	    sprintf(buf,"%s%s.xkm",xkm_output_dir,mapName);
	if (buf[0] != '\0')
	    file= fopen(buf,"rb");
	else file= NULL;
    }
    else file= NULL;
    if ((fileNameRtrn!=NULL)&&(fileNameRtrnLen>0)) {
	strncpy(fileNameRtrn,buf,fileNameRtrnLen);
	buf[fileNameRtrnLen-1]= '\0';
    }
    return file;
}

unsigned
XkbDDXLoadKeymapByNames(	DeviceIntPtr		keybd,
				XkbComponentNamesPtr	names,
				unsigned		want,
				unsigned		need,
				XkbDescPtr *		xkbRtrn,
				char *			nameRtrn,
				int 			nameRtrnLen)
{
XkbDescPtr      xkb;
FILE	*	file;
char		fileName[PATH_MAX];
unsigned	missing;

    *xkbRtrn = NULL;
    if ((keybd==NULL)||(keybd->key==NULL)||(keybd->key->xkbInfo==NULL))
	 xkb= NULL;
    else xkb= keybd->key->xkbInfo->desc;
    if ((names->keycodes==NULL)&&(names->types==NULL)&&
	(names->compat==NULL)&&(names->symbols==NULL)&&
	(names->geometry==NULL)) {
        LogMessage(X_ERROR, "XKB: No components provided for device %s\n",
                   keybd->name ? keybd->name : "(unnamed keyboard)");
        return 0;
    }
    else if (!XkbDDXCompileKeymapByNames(xkb,names,want,need,
                                         nameRtrn,nameRtrnLen)){
	LogMessage(X_ERROR, "XKB: Couldn't compile keymap\n");
	return 0;
    }
    file= XkbDDXOpenConfigFile(nameRtrn,fileName,PATH_MAX);
    if (file==NULL) {
	LogMessage(X_ERROR, "Couldn't open compiled keymap file %s\n",fileName);
	return 0;
    }
    missing= XkmReadFile(file,need,want,xkbRtrn);
    if (*xkbRtrn==NULL) {
	LogMessage(X_ERROR, "Error loading keymap %s\n",fileName);
	fclose(file);
	(void) unlink (fileName);
	return 0;
    }
    else {
	DebugF("Loaded XKB keymap %s, defined=0x%x\n",fileName,(*xkbRtrn)->defined);
    }
    fclose(file);
    return (need|want)&(~missing);
}

Bool
XkbDDXNamesFromRules(	DeviceIntPtr		keybd,
			char *			rules_name,
			XkbRF_VarDefsPtr	defs,
			XkbComponentNamesPtr	names)
{
char 		buf[PATH_MAX];
FILE *		file;
Bool		complete;
XkbRF_RulesPtr	rules;

    if (!rules_name)
	return False;

    if (strlen(XkbBaseDirectory) + strlen(rules_name) + 8 > PATH_MAX) {
        LogMessage(X_ERROR, "XKB: Rules name is too long\n");
        return False;
    }
    sprintf(buf,"%s/rules/%s", XkbBaseDirectory, rules_name);

    file = fopen(buf, "r");
    if (!file) {
        LogMessage(X_ERROR, "XKB: Couldn't open rules file %s\n", buf);
	return False;
    }

    rules = XkbRF_Create();
    if (!rules) {
        LogMessage(X_ERROR, "XKB: Couldn't create rules struct\n");
	fclose(file);
	return False;
    }

    if (!XkbRF_LoadRules(file, rules)) {
        LogMessage(X_ERROR, "XKB: Couldn't parse rules file %s\n", rules_name);
	fclose(file);
	XkbRF_Free(rules,True);
	return False;
    }

    memset(names, 0, sizeof(*names));
    complete = XkbRF_GetComponents(rules,defs,names);
    fclose(file);
    XkbRF_Free(rules, True);

    if (!complete)
        LogMessage(X_ERROR, "XKB: Rules returned no components\n");

    return complete;
}

XkbDescPtr
XkbCompileKeymap(DeviceIntPtr dev, XkbRMLVOSet *rmlvo)
{
    XkbComponentNamesRec kccgst;
    XkbRF_VarDefsRec mlvo;
    XkbDescPtr xkb;
    char name[PATH_MAX];

    if (!dev || !rmlvo) {
        LogMessage(X_ERROR, "XKB: No device or RMLVO specified\n");
        return NULL;
    }

    mlvo.model = rmlvo->model;
    mlvo.layout = rmlvo->layout;
    mlvo.variant = rmlvo->variant;
    mlvo.options = rmlvo->options;

    /* XDNFR already logs for us. */
    if (!XkbDDXNamesFromRules(dev, rmlvo->rules, &mlvo, &kccgst))
        return NULL;

    /* XDLKBN too, but it might return 0 as well as allocating. */
    if (!XkbDDXLoadKeymapByNames(dev, &kccgst, XkmAllIndicesMask, 0, &xkb, name,
                                 PATH_MAX)) {
        if (xkb)
            XkbFreeKeyboard(xkb, 0, TRUE);
        return NULL;
    }

    return xkb;
}
