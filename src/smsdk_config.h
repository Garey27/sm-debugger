
#ifndef _INCLUDE_SOURCEMOD_EXTENSION_CONFIG_H_
#define _INCLUDE_SOURCEMOD_EXTENSION_CONFIG_H_

#define SMEXT_CONF_NAME			"SM Debugger"
#define SMEXT_CONF_DESCRIPTION	"SM Debugger"
#define SMEXT_CONF_VERSION		"0.0.1"
#define SMEXT_CONF_AUTHOR		"Garey"
#define SMEXT_CONF_URL			"http://hlmod.ru"
#define SMEXT_CONF_LOGTAG		"SMD"
#define SMEXT_CONF_LICENSE		"WTFPL"
#define SMEXT_CONF_DATESTRING	__DATE__

#define SMEXT_LINK(name) SDKExtension *g_pExtensionIface = name;
#define META_NO_HL2SDK
#define SMEXT_CONF_METAMOD

#define SMEXT_ENABLE_FORWARDSYS
#define SMEXT_ENABLE_HANDLESYS
//#define SMEXT_ENABLE_PLAYERHELPERS
//#define SMEXT_ENABLE_DBMANAGER
//#define SMEXT_ENABLE_GAMECONF
//#define SMEXT_ENABLE_MEMUTILS
//#define SMEXT_ENABLE_GAMEHELPERS
//#define SMEXT_ENABLE_TIMERSYS
//#define SMEXT_ENABLE_THREADER
//#define SMEXT_ENABLE_LIBSYS
//#define SMEXT_ENABLE_MENUS
//#define SMEXT_ENABLE_ADTFACTORY
//#define SMEXT_ENABLE_PLUGINSYS
//#define SMEXT_ENABLE_ADMINSYS
//#define SMEXT_ENABLE_TEXTPARSERS
//#define SMEXT_ENABLE_USERMSGS
//#define SMEXT_ENABLE_TRANSLATOR
//#define SMEXT_ENABLE_ROOTCONSOLEMENU

#endif
