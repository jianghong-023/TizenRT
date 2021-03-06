/****************************************************************************
 *
 * Copyright 2017 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/
/****************************************************************************
 * lib/libc/signal/sig_set.c
 *
 *   Copyright (C) 2015-2016 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <signal.h>
#include <assert.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sigset
 *
 * Description:
 *   The System V sigset function is very similar to the (obsolete) POSIX
 *   signal() function except that it includes additional management of the
 *   tasks' signal mask.  This function is then simply a wrapper around
 *   signal() with this additional signal mask logic added.
 *
 *   - The sigset set function also accepts the SIG_HOLD value for 'func':
 *     If 'func' is equal to SIG_HOLD, 'sig' will be added to the signal
 *     mask of the calling process and 'sig's disposition will remain
 *     unchanged.
 *   - If 'func' is not equal to SIG_HOLD, 'sig' will be removed from the
 *     signal mask of the calling process.
 *
 * Input Parameters:
 *   sig - Identifies the signal to operate on
 *   func  - The new disposition of the signal
 *
 * Returned Value:
 *   Upon successful completion, sigset() shall return SIG_HOLD if the
 *   signal had been blocked and the signal's previous disposition if it had
 *   not been blocked. Otherwise, SIG_ERR shall be returned and errno set to
 *   indicate the error.
 *
 ****************************************************************************/

CODE void (*sigset(int sig, CODE void (*func)(int sig)))(int sig)
{
	_sa_handler_t disposition;
	sigset_t set;
	int ret;

	DEBUGASSERT(GOOD_SIGNO(sig) && func != SIG_ERR);

	(void)sigemptyset(&set);
	(void)sigaddset(&set, sig);

	/* Check if we are being asked to block the signal */

	if (func == SIG_HOLD) {
		ret = sigprocmask(SIG_BLOCK, &set, NULL);
		disposition = ret < 0 ? SIG_ERR : SIG_HOLD;
	}

	/* No.. then signal can handle the other cases */

	else {
		/* Set the signal handler disposition */

		disposition = signal(sig, func);
		if (disposition != SIG_ERR) {
			/* And unblock the signal */

			ret = sigprocmask(SIG_UNBLOCK, &set, NULL);
			if (ret < 0) {
				/* Restore the original signal disposition and return and
				 * error.
				 */

				(void)signal(sig, disposition);
				disposition = SIG_ERR;
			}
		}
	}

	return disposition;
}
