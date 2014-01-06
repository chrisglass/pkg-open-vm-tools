/*********************************************************
 * Copyright (C) 2008 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/*
 * toolboxcmd-devices.c --
 *
 *     The devices functions for toolbox-cmd
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "toolboxInt.h"
#include "toolboxCmdInt.h"
#include "vmware/tools/i18n.h"


/*
 *-----------------------------------------------------------------------------
 *
 * DevicesList  --
 *
 *      prints device names and status to stdout.
 *
 * Results:
 *      EXIT_SUCCESS.
 *
 * Side effects:
 *      Prints to stdout.
 *
 *-----------------------------------------------------------------------------
 */

static int
DevicesList(void)
{
   int i;
   for (i = 0; i < MAX_DEVICES; i++) {
      RD_Info info;
      if (GuestApp_GetDeviceInfo(i, &info) && strlen(info.name) > 0) {
         printf ("%s: %s\n", info.name, info.enabled ? "Enabled" : "Disabled");
      }
   }
   return EXIT_SUCCESS;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DevicesGetStatus  --
 *
 *      Prints device names to stdout.
 *
 * Results:
 *      Returns EXIT_SUCCESS on success
 *      Returns EXIT_OSFILE if devName was not found
 *
 * Side effects:
 *      Print to stderr on error.
 *
 *-----------------------------------------------------------------------------
 */

static int
DevicesGetStatus(char *devName)  // IN: Device Name
{
   int i;
   for (i = 0; i < MAX_DEVICES; i++) {
      RD_Info info;
      if (GuestApp_GetDeviceInfo(i, &info)
          && toolbox_strcmp(info.name, devName) == 0) {
         printf("%s\n", info.enabled ? "Enabled" : "Disabled");
         return EXIT_SUCCESS;
      }
   }
   fprintf(stderr,
            "error fetching interface information: Device not found\n");
   return EX_OSFILE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DevicesSetStatus  --
 *
 *      Sets device status to the value in enable.
 *
 * Results:
 *      EXIT_SUCCESS on success
 *      EXIT_TEMPFAIL on failure to connect/disconnect a device
 *      EXIT_OSFILE if device is not found
 *
 * Side effects:
 *      Possibly connects or disconnects a device.
 *      Print to stderr on error.
 *
 *-----------------------------------------------------------------------------
 */

static int
DevicesSetStatus(char *devName,  // IN: device name
                 Bool enable,    // IN: status
                 gboolean quiet) // IN: Verbosity flag
{
   int dev_id;
   for (dev_id = 0; dev_id < MAX_DEVICES; dev_id++) {
      RD_Info info;
      if (GuestApp_GetDeviceInfo(dev_id, &info)
	  && toolbox_strcmp(info.name, devName) == 0) {
         if (!GuestApp_SetDeviceState(dev_id, enable)) {
            fprintf(stderr, "Unable to %s device %s\n", enable ? "connect"
                    : "disconnect", info.name);
            return EX_TEMPFAIL;
         }
         goto exit;
      }
   }
   fprintf(stderr,
           "error fetching interface information: Device not found\n");
   return EX_OSFILE;
  exit:
   if (!quiet) {
      printf("%s\n", enable ? "Enabled" : "Disabled");
   }
   return EXIT_SUCCESS;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Device_Command --
 *
 *      Handle and parse device commands.
 *
 * Results:
 *      Returns EXIT_SUCCESS on success.
 *      Returns the exit code on errors.
 *
 * Side effects:
 *      Might enable or disable a device.
 *
 *-----------------------------------------------------------------------------
 */

int
Device_Command(char **argv,    // IN: Command line arguments
               int argc,       // IN: Length of command line arguments
               gboolean quiet) // IN
{
   char *subcommand = argv[optind];
   Bool haveDeviceArg = optind + 1 < argc;

   if (toolbox_strcmp(subcommand, "list") == 0) {
      return DevicesList();
   } else if (toolbox_strcmp(subcommand, "status") == 0) {
      if (haveDeviceArg) {
         return DevicesGetStatus(argv[optind + 1]);
      }
   } else if (toolbox_strcmp(subcommand, "enable") == 0) {
      if (haveDeviceArg) {
         return DevicesSetStatus(argv[optind + 1], TRUE, quiet);
      }
   } else if (toolbox_strcmp(subcommand, "disable") == 0) {
      if (haveDeviceArg) {
         return DevicesSetStatus(argv[optind + 1], FALSE, quiet);
      }
   } else {
      ToolsCmd_UnknownEntityError(argv[0],
                                  SU_(arg.subcommand, "subcommand"),
                                  subcommand);
      return EX_USAGE;
   }

   ToolsCmd_MissingEntityError(argv[0], SU_(arg.devicename, "device name"));
   return EX_USAGE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Device_Help --
 *
 *      Prints the help for device commands.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
Device_Help(const char *progName, // IN: The name of the program obtained from argv[0]
            const char *cmd)      // IN
{
   g_print(SU_(help.device, "%s: functions related to the virtual machine's hardware devices\n"
                            "Usage: %s %s <subcommand> [args]\n"
                            "dev is the name of the device.\n"
                            "\n"
                            "Subcommands:\n"
                            "   enable <dev>: enable the device dev\n"
                            "   disable <dev>: disable the device dev\n"
                            "   list: list all available devices\n"
                            "   status <dev>: print the status of a device\n"),
           cmd, progName, cmd);
}
