// 
//
//  $Id: spamass-milter.cpp,v 1.38 2003/06/06 16:37:24 dnelson Exp $
//
//  SpamAss-Milter 
//    - a rather trivial SpamAssassin Sendmail Milter plugin
//
//  for information about SpamAssassin please see
//                        http://www.spamassassin.org
//
//  for information about Sendmail please see
//                        http://www.sendmail.org
//
//  Copyright (c) 2002 Georg C. F. Greve <greve@gnu.org>,
//   all rights maintained by FSF Europe e.V., 
//   Villa Vogelsang, Antonienallee 1, 45279 Essen, Germany
//

// {{{ License, Contact, Notes & Includes 

//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation; either version 2 of the License, or
//   (at your option) any later version.
//  
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//  
//   You should have received a copy of the GNU General Public License
//   along with this program; if not, write to the Free Software
//   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
//   Contact:
//            Michael Brown <michaelb@opentext.com>
//

// Notes:
//
//  The libmilter for sendmail works callback-oriented, so if you have no
//  experience with event-driven programming, the following may be hard for
//  you to understand.
//
//  The code should be reasonably thread-safe. No guarantees, though.
//
//  This program roughly does the following steps:
//
//   1. register filter with libmilter & set up socket
//   2. register the callback functions defined in this file
//    -- wait for mail to show up --
//   3. start spamc client
//   4. assemble mail since libmilter passes it in pieces and put
//      these parts in the output pipe to spamc.
//   5. when the mail is complete, close the pipe.
//   6. read output from spamc, close input pipe and clean up PID
//   7. check for the flags affected by SpamAssassin and set/change
//      them accordingly
//   8. replace the body with the one provided by SpamAssassin if the
//      mail was rated spam, unless -m is specified
//   9. free all temporary data
//   10. tell sendmail to let the mail to go on (default) or be discarded
//    -- wait for mail to show up -- (restart at 3)
//

// Includes  
#include "config.h"

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <signal.h>
#ifdef HAVE_POLL_H
#include <poll.h>
#else
#include "subst_poll.h"
#endif
#include <errno.h>

// C++ includes
#include <cstdio>
#include <cstddef>
#include <csignal>
#include <string>
#include <iostream>

#ifdef  __cplusplus
extern "C" {
#endif

#include "libmilter/mfapi.h"
//#include "libmilter/mfdef.h"

#ifdef  __cplusplus
}
#endif

#include "spamass-milter.h"

// }}} 

static const char Id[] = "$Id: spamass-milter.cpp,v 1.38 2003/06/06 16:37:24 dnelson Exp $";

struct smfiDesc smfilter =
  {
    "SpamAssassin", // filter name
    SMFI_VERSION,   // version code -- leave untouched
    SMFIF_ADDHDRS|SMFIF_CHGHDRS|SMFIF_CHGBODY,  // flags
    mlfi_connect, // info filter callback
    NULL, // HELO filter callback
    mlfi_envfrom, // envelope sender filter callback
    mlfi_envrcpt, // envelope recipient filter callback
    mlfi_header, // header filter callback
    mlfi_eoh, // end of header callback
    mlfi_body, // body filter callback
    mlfi_eom, // end of message callback
    mlfi_abort, // message aborted callback
    mlfi_close, // connection cleanup callback
  };

const char *const debugstrings[] = {
	"ALL", "FUNC", "POLL", "UORI", "STR", "MISC", "NET", "SPAMC",
	NULL
};

int flag_debug = (1<<D_ALWAYS);
bool flag_reject = false;
bool flag_sniffuser = false;
int reject_score = -1;
bool dontmodify = false;
char *defaultuser;
char *spamdhost;
struct networklist ignorenets;
int spamc_argc;
char **spamc_argv;
// {{{ main()

int
main(int argc, char* argv[])
{
   int c, err = 0;
   const char *args = "p:fd:mr:u:D:i:";
   char *sock = NULL;
   bool dofork = false;

   openlog("spamass-milter", LOG_PID, LOG_MAIL);

	/* Process command line options */
	while ((c = getopt(argc, argv, args)) != -1) {
		switch (c) {
			case 'p':
				sock = strdup(optarg);
				break;
			case 'f':
				dofork = true;
				break;
			case 'd':
				parse_debuglevel(optarg);
				break;
			case 'D':
				spamdhost = strdup(optarg);
				break;
			case 'i':
				debug(D_MISC, "Parsing ignore list");
				parse_networklist(optarg, &ignorenets);
				break;
			case 'm':
				dontmodify = true;
				break;
			case 'r':
				flag_reject = true;
				reject_score = atoi(optarg);
				break;
			case 'u':
				flag_sniffuser = true;
				defaultuser = strdup(optarg);
				break;
			case '?':
				err = 1;
				break;
		}
	}

   /* remember the remainer of the arguments so we can pass them to spamc */
   spamc_argc = argc - optind;
   spamc_argv = argv + optind;

   if (!sock || err) {
      cout << PACKAGE_NAME << " - Version " << PACKAGE_VERSION << endl;
      cout << "SpamAssassin Sendmail Milter Plugin" << endl;
      cout << "Usage: spamass-milter -p socket [-d nn] [-D host] [-f] [-i networks]" << endl;
      cout << "                      [-m] [-r nn] [-u defaultuser] [-- spamc args ]" << endl;
      cout << "   -p socket: path to create socket" << endl;
      cout << "   -d xx[,yy ...]: set debug flags.  Logs to syslog" << endl;
      cout << "   -D host: connect to spand at remote host (deprecated)" << endl;
      cout << "   -f: fork into background" << endl;
      cout << "   -i: skip (ignore) checks from these IPs or netblocks" << endl;
      cout << "       example: -i 192.168.12.5,10.0.0.0/8,172.16/255.255.0.0" << endl;
      cout << "   -m: don't modify body, Content-type: or Subject:" << endl;
      cout << "   -r nn: reject messages with a score >= nn with an SMTP error.\n"
              "          use -1 to reject any messages tagged by SA." << endl;
      cout << "   -u defaultuser: pass the recipient's username to spamc.\n"
              "          Uses 'defaultuser' if there are multiple recipients." << endl;
      cout << "   -- spamc args: pass the remaining flags to spamc.\n" << endl;
              
      exit(EX_USAGE);
   }
  
	if (dofork == true) {
		switch(fork()) {
         case -1: /* Uh-oh, we have a problem forking. */
            fprintf(stderr, "Uh-oh, couldn't fork!\n");
				exit(errno);
				break;
			case 0: /* Child */
				break;
			default: /* Parent */
				exit(0);
		}
	}
   {
      struct stat junk;
      if (stat(sock,&junk) == 0) unlink(sock);
   }


   (void) smfi_setconn(sock);
	if (smfi_register(smfilter) == MI_FAILURE) {
		fprintf(stderr, "smfi_register failed\n");
		exit(EX_UNAVAILABLE);
	} else {
      debug(D_MISC, "smfi_register succeeded");
   }
	debug(D_ALWAYS, "spamass-milter %s starting", PACKAGE_VERSION);
	err = smfi_main();
	debug(D_ALWAYS, "spamass-milter %s exiting", PACKAGE_VERSION);
	return err;
}

// }}}

/* Update a header if SA changes it, or add it if it is new. */
void update_or_insert(SpamAssassin* assassin, SMFICTX* ctx, string oldstring, t_setter setter, char *header )
{
	string::size_type eoh1(assassin->d().find("\n\n"));
	string::size_type eoh2(assassin->d().find("\n\r\n"));
	string::size_type eoh = ( eoh1 < eoh2 ? eoh1 : eoh2 );

	string newstring;
	string::size_type oldsize;

	debug(D_UORI, "u_or_i: looking at <%s>", header);
	debug(D_UORI, "u_or_i: oldstring: <%s>", oldstring.c_str());

	newstring = retrieve_field(assassin->d().substr(0, eoh), string(header));
	debug(D_UORI, "u_or_i: newstring: <%s>", newstring.c_str());

	oldsize = callsetter(*assassin,setter)(newstring);
      
	if (newstring != oldstring)
	{
		/* change if old one was present, append if non-null */
		if (oldsize > 0)
		{
			debug(D_UORI, "u_or_i: changing");
			smfi_chgheader(ctx, header, 1, newstring.size() > 0 ? 
				const_cast<char*>(newstring.c_str()) : NULL );
		} else if (newstring.size() > 0)
		{
			debug(D_UORI, "u_or_i: inserting");
			smfi_addheader(ctx, header, 
				const_cast<char*>(newstring.c_str()));
		}
	} else
	{
		debug(D_UORI, "u_or_i: no change");
	}
}

// {{{ Assassinate

//
// implement the changes suggested by SpamAssassin for the mail.  Returns
// the milter error code.
int 
assassinate(SMFICTX* ctx, SpamAssassin* assassin)
{
  // find end of header (eol in last line of header)
  // and beginning of body
  string::size_type eoh1(assassin->d().find("\n\n"));
  string::size_type eoh2(assassin->d().find("\n\r\n"));
  string::size_type eoh = ( eoh1 < eoh2 ? eoh1 : eoh2 );
  string::size_type bob = assassin->d().find_first_not_of("\r\n", eoh);

  update_or_insert(assassin, ctx, assassin->spam_flag(), &SpamAssassin::set_spam_flag, "X-Spam-Flag");
  update_or_insert(assassin, ctx, assassin->spam_status(), &SpamAssassin::set_spam_status, "X-Spam-Status");

  /* Summarily reject the message if SA tagged it, or if we have a minimum
     score, reject if it exceeds that score. */
  if (flag_reject)
  {
	bool do_reject = false;
	if (reject_score == -1 && assassin->spam_flag().size()>0)
		do_reject = true;
	if (reject_score != -1)
	{
		int score, rv;
		const char *spam_status = assassin->spam_status().c_str();
		rv = sscanf(spam_status,"%*s hits=%d", &score);
		if (rv != 1)
			debug(D_ALWAYS, "Could not extract score from <%s>", spam_status);
		else 
		{
			debug(D_MISC, "SA score: %d", score);
			if (score >= reject_score)
				do_reject = true;
		}
	}
	if (do_reject)
	{
		debug(D_MISC, "Rejecting");
		smfi_setreply(ctx, "550", "5.7.1", "Blocked by SpamAssassin");
		return SMFIS_REJECT;
	}
  }

  update_or_insert(assassin, ctx, assassin->spam_report(), &SpamAssassin::set_spam_report, "X-Spam-Report");
  update_or_insert(assassin, ctx, assassin->spam_prev_content_type(), &SpamAssassin::set_spam_prev_content_type, "X-Spam-Prev-Content-Type");
  update_or_insert(assassin, ctx, assassin->spam_level(), &SpamAssassin::set_spam_level, "X-Spam-Level");
  update_or_insert(assassin, ctx, assassin->spam_checker_version(), &SpamAssassin::set_spam_checker_version, "X-Spam-Checker-Version");

  // 
  // If SpamAssassin thinks it is spam, replace
  //  Subject:
  //  Content-Type:
  //  <Body>
  // 
  //  However, only issue the header replacement calls if the content has
  //  actually changed. If SA didn't change subject or content-type, don't
  //  replace here unnecessarily.
  if (!dontmodify && assassin->spam_flag().size()>0)
    {
	  update_or_insert(assassin, ctx, assassin->subject(), &SpamAssassin::set_subject, "Subject");
	  update_or_insert(assassin, ctx, assassin->content_type(), &SpamAssassin::set_content_type, "Content-Type");

      // Replace body with the one SpamAssassin provided
      string::size_type body_size = assassin->d().size() - bob;
      string body=assassin->d().substr(bob, string::npos);
      if ( smfi_replacebody(ctx, (unsigned char *)body.c_str(), body_size) == MI_FAILURE )
	throw string("error. could not replace body.");
      
    };

  return SMFIS_CONTINUE;
}

// retrieve the content of a specific field in the header
// and return it.
string
old_retrieve_field(const string& header, const string& field)
{
  // look for beginning of content
  string::size_type pos = find_nocase(header, string("\n")+field+string(": "));

  // return empty string if not found
  if (pos == string::npos)
  {
    debug(D_STR, "r_f: failed");
    return string("");
  }

  // look for end of field name
  pos = find_nocase(header, string(" "), pos) + 1;
  
  string::size_type pos2(pos);

  // is field empty? 
  if (pos2 == find_nocase(header, string("\n"), pos2))
    return string("");

  // look for end of content
  do {

    pos2 = find_nocase(header, string("\n"), pos2+1);

  }
  while ( pos2 < string::npos &&
	  isspace(header[pos2+1]) );

  return header.substr(pos, pos2-pos);

}

// retrieve the content of a specific field in the header
// and return it.
string
retrieve_field(const string& header, const string& field)
{
  // Find the field
  string::size_type field_start = string::npos;
  string::size_type field_end = string::npos;
  string::size_type idx = 0;

  while( field_start == string::npos ) {
	idx = find_nocase( header, field + string(":"), idx );

	// no match
	if ( idx == string::npos ) {
	  return string( "" );
	}

	// The string we've found needs to be either at the start of the
	// headers string, or immediately following a "\n"
	if ( idx != 0 ) {
	  if ( header[ idx - 1 ] != '\n' ) {
		idx++; // so we don't get stuck in an infinite loop
		continue; // loop around again
	  }
	}

	field_start = idx;
  }

  // A mail field starts just after the header. Ideally, there's a
  // space, but it's possible that there isn't.
  field_start += field.length() + 1;
  if ( field_start < ( header.length() - 1 ) && header[ field_start ] == ' ' ) {
	field_start++;
  }

  // See if there's anything left, to shortcut the rest of the
  // function.
  if ( field_start == header.length() - 1 ) {
	return string( "" );
  }

  // The field continues to the end of line. If the start of the next
  // line is whitespace, then the field continues.
  idx = field_start;
  while( field_end == string::npos ) {
	idx = header.find( "\n", idx );

	// if we don't find a "\n", gobble everything to the end of the headers
	if ( idx == string::npos ) {
	  field_end = header.length();
	} else {
	  // check the next character
	  if (( idx + 1 ) < header.length() && ( isspace( header[ idx + 1 ] ))) {
		idx ++; // whitespace found, so wrap to the next line
	  } else {
		field_end = idx;
	  }
	}
  }

  //  Maybe remove the whitespace picked up when a header wraps - this
  //  might actually be a requirement
  return header.substr( field_start, field_end - field_start );
}


// }}}

// {{{ MLFI callbacks

//
// Gets called once when a client connects to sendmail
//
// gets the originating IP address and checks it against the ignore list
// if it isn't in the list, store the IP in a structure and store a 
// pointer to it in the private data area.
//
sfsistat 
mlfi_connect(SMFICTX * ctx, char *hostname, _SOCK_ADDR * hostaddr)
{
	struct connect_info *ci;

	debug(D_FUNC, "mlfi_connect: enter");

	/* allocate a structure to store the IP address in */
	ci = (struct connect_info *)malloc(sizeof(*ci));
	ci->ip = ((struct sockaddr_in *) hostaddr)->sin_addr;
	
	/* store a pointer to it with setpriv */
	smfi_setpriv(ctx, ci);

	if (ip_in_networklist(((struct sockaddr_in *) hostaddr)->sin_addr, &ignorenets))
	{
		debug(D_NET, "%s is in our ignore list - accepting message",
		    inet_ntoa(((struct sockaddr_in *) hostaddr)->sin_addr));
		debug(D_FUNC, "mlfi_connect: exit ignore");
		return SMFIS_ACCEPT;
	}
	
	// Tell Milter to continue
	debug(D_FUNC, "mlfi_connect: exit");

	return SMFIS_CONTINUE;
}


//
// Gets called first for all messages
//
// creates SpamAssassin object and makes pointer to it
// private data of this filter process
//
sfsistat
mlfi_envfrom(SMFICTX* ctx, char** envfrom)
{
  SpamAssassin* assassin;

  debug(D_FUNC, "mlfi_envfrom: enter");
  try {
    // launch new SpamAssassin
    assassin=new SpamAssassin;
  } catch (string& problem)
    {
      throw_error(problem);
      return SMFIS_TEMPFAIL;
    };
  
  /* grab and record the pointer to our connection-specific data before it
     gets overwritten */

  assassin->ci = (struct connect_info*)smfi_getpriv(ctx);
  assassin->set_connectip(string(inet_ntoa(assassin->ci->ip)));
  
  // register the pointer to SpamAssassin object as private data
  smfi_setpriv(ctx, static_cast<void*>(assassin));

  // remember the MAIL FROM address
  assassin->set_from(string(envfrom[0]));

  // tell Milter to continue
  debug(D_FUNC, "mlfi_envfrom: exit");

  return SMFIS_CONTINUE;
}

//
// Gets called once for each recipient
//
// stores the first recipient in the spamassassin object and
// discards the rest, keeping track of the number of recipients.
//
sfsistat
mlfi_envrcpt(SMFICTX* ctx, char** envrcpt)
{
	SpamAssassin* assassin = static_cast<SpamAssassin*>(smfi_getpriv(ctx));

	if (assassin->numrcpt() == 0)
	{
		assassin->set_numrcpt(1);
		assassin->set_rcpt(string(envrcpt[0]));

		// Check if the SPAMC program has already been run, if not we run it.
		if ( !(assassin->connected) )
		{
			try {
				assassin->connected = 1; // SPAMC is getting ready to run
				assassin->Connect();

				/* Send the envelope headers as X-Envelope-From: and
				   X-Envelope-To: so that SpamAssassin can use them in its
				   whitelist checks.  Also forge a dummy Received: header
				   because SA gets the connecting IP from the topmost one
				*/

				assassin->output("X-Envelope-From: ");
				assassin->output(assassin->from());
				assassin->output("\r\nX-Envelope-To: ");
				assassin->output(assassin->rcpt());
				assassin->output("\r\nReceived: from [");
				assassin->output(assassin->connectip());
				assassin->output("]\r\n");
			} 
			catch (string& problem) {
				throw_error(problem);
				return SMFIS_TEMPFAIL;
			};
		}
	} else
	{
		assassin->set_numrcpt();
	}
	return SMFIS_CONTINUE;
}

//
// Gets called repeatedly for all header fields
//
// assembles the headers and passes them on to the SpamAssassin client
// through the pipe.
//
// only exception: SpamAssassin header fields (X-Spam-*) get suppressed
// but are being stored in the SpamAssassin element.
//
// this function also starts the connection with the SPAMC program the
// first time it is called.
//

sfsistat
mlfi_header(SMFICTX* ctx, char* headerf, char* headerv)
{
  SpamAssassin* assassin = static_cast<SpamAssassin*>(smfi_getpriv(ctx));
  debug(D_FUNC, "mlfi_header: enter");


  // Is it a "X-Spam-" header field?
  if ( cmp_nocase_partial(string("X-Spam-"), string(headerf)) == 0 )
    {
      int suppress = 1;
      // memorize content of old fields

      if ( cmp_nocase_partial(string("X-Spam-Status"), string(headerf)) == 0 )
	assassin->set_spam_status(string(headerv));
      else if ( cmp_nocase_partial(string("X-Spam-Flag"), string(headerf)) == 0 )
	assassin->set_spam_flag(string(headerv));
      else if ( cmp_nocase_partial(string("X-Spam-Report"), string(headerf)) == 0 )
	assassin->set_spam_report(string(headerv));
      else if ( cmp_nocase_partial(string("X-Spam-Prev-Content-Type"), string(headerf)) == 0 )
	assassin->set_spam_prev_content_type(string(headerv));
      else if ( cmp_nocase_partial(string("X-Spam-Level"), string(headerf)) == 0 )
	assassin->set_spam_level(string(headerv));
      else if ( cmp_nocase_partial(string("X-Spam-Checker-Version"), string(headerf)) == 0 )
	assassin->set_spam_checker_version(string(headerv));
      else
      {
      	/* Hm. X-Spam header, but not one we recognize.  Pass it through. */
      	suppress = 0;
      }
      
      if (suppress)
      {
	debug(D_FUNC, "mlfi_header: suppress");
	return SMFIS_CONTINUE;
      }
    };

  // Content-Type: will be stored if present
  if ( cmp_nocase_partial(string("Content-Type"), string(headerf)) == 0 )
    assassin->set_content_type(string(headerv));

  // Subject: should be stored
  if ( cmp_nocase_partial(string("Subject"), string(headerf)) == 0 )
    assassin->set_subject(string(headerv));

  // assemble header to be written to SpamAssassin
  string header=string(headerf)+string(": ")+
    string(headerv)+string("\r\n");
 
  try {
    // write to SpamAssassin client
    assassin->output(header.c_str(),header.size());
  } catch (string& problem)
    {
      throw_error(problem);
      smfi_setpriv(ctx, assassin->ci);
      delete assassin;
      debug(D_FUNC, "mlfi_header: exit error");
      return SMFIS_TEMPFAIL;
    };
  
  // go on...
  debug(D_FUNC, "mlfi_header: exit");

  return SMFIS_CONTINUE;
}

// 
// Gets called once when the header is finished.
//
// writes empty line to SpamAssassin client to separate
// headers from body.
//
sfsistat
mlfi_eoh(SMFICTX* ctx)
{
  SpamAssassin* assassin = static_cast<SpamAssassin*>(smfi_getpriv(ctx));

  debug(D_FUNC, "mlfi_eoh: enter");

  // Check if the SPAMC program has already been run, if not we run it.
  if ( !(assassin->connected) )
     {
       try {
         assassin->connected = 1; // SPAMC is getting ready to run
         assassin->Connect();
       } 
       catch (string& problem) {
         throw_error(problem);
         return SMFIS_TEMPFAIL;
       };
     }

  try {
    // add blank line between header and body
    assassin->output("\r\n",2);
  } catch (string& problem)
    {
      throw_error(problem);
      smfi_setpriv(ctx, assassin->ci);
      delete assassin;
  
      debug(D_FUNC, "mlfi_eoh: exit error");
      return SMFIS_TEMPFAIL;
    };
  
  // go on...

  debug(D_FUNC, "mlfi_eoh: exit");
  return SMFIS_CONTINUE;
}

//
// Gets called repeatedly to transmit the body
//
// writes everything directly to SpamAssassin client
//
sfsistat
mlfi_body(SMFICTX* ctx, u_char *bodyp, size_t bodylen)
{
  debug(D_FUNC, "mlfi_body: enter");
  SpamAssassin* assassin = static_cast<SpamAssassin*>(smfi_getpriv(ctx));
 
  try {
    assassin->output(bodyp, bodylen);
  } catch (string& problem)
    {
      throw_error(problem);
      smfi_setpriv(ctx, assassin->ci);
      delete assassin;
      debug(D_FUNC, "mlfi_body: exit error");
      return SMFIS_TEMPFAIL;
    };

  // go on...
  debug(D_FUNC, "mlfi_body: exit");
  return SMFIS_CONTINUE;
}

//
// Gets called once at the end of mail processing
//
// tells SpamAssassin client that the mail is complete
// through EOF and then modifies the mail accordingly by
// calling the "assassinate" function
//
sfsistat
mlfi_eom(SMFICTX* ctx)
{
  SpamAssassin* assassin = static_cast<SpamAssassin*>(smfi_getpriv(ctx));
  int milter_status;
 
  debug(D_FUNC, "mlfi_eom: enter");
  try {

    // close output pipe to signal EOF to SpamAssassin
    assassin->close_output();

    // read what the Assassin is telling us
    assassin->input();

    milter_status = assassinate(ctx, assassin);

    // now cleanup the element.
    smfi_setpriv(ctx, assassin->ci);
    delete assassin;

  } catch (string& problem)
    {
      throw_error(problem);
      smfi_setpriv(ctx, assassin->ci);
      delete assassin;
      debug(D_FUNC, "mlfi_eom: exit error");
      return SMFIS_TEMPFAIL;
    };
  
  // go on...
  debug(D_FUNC, "mlfi_eom: exit");
  return milter_status;
}

//
// Gets called on session-basis. This keeps things nice & quiet.
//
sfsistat
mlfi_close(SMFICTX* ctx)
{
  struct connect_info *ci;
  debug(D_FUNC, "mlfi_close");
  
  ci = (struct connect_info*)smfi_getpriv(ctx);
  if (ci == NULL)
  {
    /* the context should have been set in mlfi_connect */
  	debug(D_ALWAYS, "NULL context in mlfi_close! Should not happen!");
    return SMFIS_ACCEPT;
  }
  free(ci);
  smfi_setpriv(ctx, NULL);
  
  return SMFIS_ACCEPT;
}

//
// Gets called when things are being aborted.
//
// kills the SpamAssassin object, its destructor should
// take care of everything.
//
sfsistat
mlfi_abort(SMFICTX* ctx)
{
  SpamAssassin* assassin = static_cast<SpamAssassin*>(smfi_getpriv(ctx));

  debug(D_FUNC, "mlfi_abort");
  smfi_setpriv(ctx, assassin->ci);
  delete assassin;

  return SMFIS_ACCEPT;
}

// }}}

// {{{ SpamAssassin Class

//
// This is a new constructor for the SpamAssassin object.  It simply 
// initializes two variables.  The original constructor has been
// renamed to Connect().
//
SpamAssassin::SpamAssassin():
  error(false),
  connected(false),
  _numrcpt(0)
{
}


SpamAssassin::~SpamAssassin()
{ 
	if (connected) 
	{
		// close all pipes that are still open
		if (pipe_io[0][0] > -1)	close(pipe_io[0][0]);
		if (pipe_io[0][1] > -1)	close(pipe_io[0][1]);
		if (pipe_io[1][0] > -1)	close(pipe_io[1][0]);
		if (pipe_io[1][1] > -1)	close(pipe_io[1][1]);

		// child still running?
		if (running)
		{
			// slaughter child
			kill(pid, SIGKILL);

			// wait for child to terminate
			int status;
			waitpid(pid, &status, 0);
		}
	}
}

//
// This is the old SpamAssassin constructor.  It has been renamed Connect(),
// and is now called at the beginning of the mlfi_header() function.
//

void SpamAssassin::Connect()
{
  // set up pipes for in- and output
  if (pipe(pipe_io[0]))
    throw string(string("pipe error: ")+string(strerror(errno)));
  if (pipe(pipe_io[1]))
    throw string(string("pipe error: ")+string(strerror(errno)));
  
  // now execute SpamAssassin client for contact with SpamAssassin spamd

  // start child process
  switch(pid = fork())
    {
    case -1:
      // forking trouble. throw error.
      throw string(string("fork error: ")+string(strerror(errno)));
      break;
    case 0:
      // +++ CHILD +++
      
      // close unused pipes
      close(pipe_io[1][0]);
      close(pipe_io[0][1]);

      // redirect stdin(0), stdout(1) and stderr(2)
      dup2(pipe_io[0][0],0);
      dup2(pipe_io[1][1],1);
      dup2(pipe_io[1][1],2);

      closeall(3);

      // execute spamc 
      // absolute path (determined in autoconf) 
      // should be a little more secure
      // XXX arbitrary 100-argument max
      int argc = 0;
      char** argv = (char**) malloc(100*sizeof(char*));
      argv[argc++] = SPAMC;
      if (flag_sniffuser) 
      {
        argv[argc++] = "-u";
        if ( numrcpt() != 1 )
        {
          // More (or less?) than one recipient, so we pass the default
          // username to SPAMC.  This way special rules can be defined for
          // multi recipient messages.
          argv[argc++] = defaultuser; 
        } else
        { 
          // There is only 1 recipient so we pass the username to SPAMC 
          argv[argc++] = (char *) local_user().c_str(); 
        }
      }
      if (spamdhost) 
      {
        argv[argc++] = "-d";
        argv[argc++] = spamdhost;
      }
      if (spamc_argc)
      {
      	memcpy(argv+argc, spamc_argv, spamc_argc * sizeof(char *));
      	argc += spamc_argc;
      }
      argv[argc++] = 0;

      execvp(argv[0] , argv); // does not return!

      // execution failed
      throw_error(string("execution error: ")+string(strerror(errno)));
      
      break;
    }

  // +++ PARENT +++

  // close unused pipes
  close(pipe_io[0][0]);
  close(pipe_io[1][1]);
  pipe_io[0][0]=-1;
  pipe_io[1][1]=-1;

  // mark the pipes non-blocking
  if(fcntl(pipe_io[0][1], F_SETFL, O_NONBLOCK) == -1)
     throw string(string("Cannot set pipe01 nonblocking: ")+string(strerror(errno)));
#if 0  /* don't really need to make the sink pipe nonblocking */
  if(fcntl(pipe_io[1][0], F_SETFL, O_NONBLOCK) == -1)
     throw string(string("Cannot set pipe10 nonblocking: ")+string(strerror(errno)));
#endif

  // we have to assume the client is running now.
  running=true;

}

// write to SpamAssassin
void
SpamAssassin::output(const void* buffer, long size)
{
  debug(D_FUNC, "::output enter");

  debug(D_SPAMC, "output \"%*.*s\"", (int)size, (int)size, (char *)buffer);

  // if there are problems, fail.
  if (!running || error)
    throw string("tried output despite problems. failed.");

  // send to SpamAssassin
  long total(0), wsize(0);
  string reason;
  int status;
  do {
	struct pollfd fds[2];
	int nfds = 2, nready;
	fds[0].fd = pipe_io[0][1];
	fds[0].events = POLLOUT;
	fds[1].fd = pipe_io[1][0];
	fds[1].events = POLLIN;

	debug(D_POLL, "polling fds %d and %d", pipe_io[0][1], pipe_io[1][0]);
	nready = poll(fds, nfds, 1000);
	if (nready == -1)
		throw("poll failed");

	debug(D_POLL, "poll returned %d, fd0=%d, fd1=%d", nready, fds[0].revents, fds[1].revents);

	if (fds[1].revents & POLLIN)
	{
		debug(D_POLL, "poll says I can read");
		read_pipe();
	}

	if (fds[0].revents & POLLOUT)
	{
		debug(D_POLL, "poll says I can write");
		switch(wsize = write(pipe_io[0][1], (char *)buffer + total, size - total))
		{
		  case -1:
			if (errno == EAGAIN)
				continue;
			reason = string(strerror(errno));

			// close the pipes
			close(pipe_io[0][1]);
			close(pipe_io[1][0]);
			pipe_io[0][1]=-1;	
			pipe_io[1][0]=-1;	

			// Slaughter child
			kill(pid, SIGKILL);

			// set flags
			error = true;
			running = false;
	
			// wait until child is dead
			waitpid(pid, &status, 0);

			throw string(string("write error: ")+reason);	
			break;
	      default:
			total += wsize;
			debug(D_POLL, "wrote %ld bytes", wsize);
			break;
		}
	}
  } while ( total < size );

  debug(D_FUNC, "::output exit");
}

void SpamAssassin::output(const void* buffer)
{
	output(buffer, strlen((const char *)buffer));
}

void SpamAssassin::output(string buffer)
{
	output(buffer.c_str(), buffer.size());
}

// close output pipe
void
SpamAssassin::close_output()
{
  if(close(pipe_io[0][1]))
    throw string(string("close error: ")+string(strerror(errno)));
  pipe_io[0][1]=-1;
}

void
SpamAssassin::input()
{
	debug(D_FUNC, "::input enter");
  // if the child has exited or we experienced an error, return
  // immediately.
  if (!running || error)
    return;

  // keep reading from input pipe until it is empty
  empty_and_close_pipe();
  
  // that's it, we're through
  running = false;

  // wait until child is dead
  int status;
  if(waitpid(pid, &status, 0)<0)
    {
      error = true;
      throw string(string("waitpid error: ")+string(strerror(errno)));
    }; 
	debug(D_FUNC, "::input exit");
}

//
// return reference to mail
//
string& 
SpamAssassin::d()
{
  return mail;
}

//
// get values of the different SpamAssassin fields
//
string& 
SpamAssassin::spam_status()
{
  return x_spam_status;
}

string& 
SpamAssassin::spam_flag()
{
  return x_spam_flag;
}

string& 
SpamAssassin::spam_report()
{
  return x_spam_report;
}

string& 
SpamAssassin::spam_prev_content_type()
{
  return x_spam_prev_content_type;
}

string& 
SpamAssassin::spam_checker_version()
{
  return x_spam_checker_version;
}

string& 
SpamAssassin::spam_level()
{
  return x_spam_level;
}

string& 
SpamAssassin::content_type()
{
  return _content_type;
}

string& 
SpamAssassin::subject()
{
  return _subject;
}

string&
SpamAssassin::rcpt()
{
  return _rcpt;
}

string&
SpamAssassin::from()
{
  return _from;
}

string&
SpamAssassin::connectip()
{
  return _connectip;
}


string
SpamAssassin::local_user()
{
  // assuming we have a recipient in the form: <username@somehost.somedomain>
  // we return 'username'
  return _rcpt.substr(1,_rcpt.find('@')-1);
}

int
SpamAssassin::numrcpt()
{
  return _numrcpt;
}

int
SpamAssassin::set_numrcpt()
{
  _numrcpt++;
  return _numrcpt;
}

int
SpamAssassin::set_numrcpt(const int val)
{
  _numrcpt = val;
  return _numrcpt;
}

//
// set the values of the different SpamAssassin
// fields in our element. Returns former size of field
//
string::size_type
SpamAssassin::set_spam_status(const string& val)
{
  string::size_type old = x_spam_status.size();
  x_spam_status = val;
  return (old);
}

string::size_type
SpamAssassin::set_spam_flag(const string& val)
{
  string::size_type old = x_spam_flag.size();
  x_spam_flag = val;
  return (old);
}

string::size_type
SpamAssassin::set_spam_report(const string& val)
{
  string::size_type old = x_spam_report.size();
  x_spam_report = val;
  return (old);
}

string::size_type
SpamAssassin::set_spam_prev_content_type(const string& val)
{
  string::size_type old = x_spam_prev_content_type.size();
  x_spam_prev_content_type = val;
  return (old);
}

string::size_type
SpamAssassin::set_spam_checker_version(const string& val)
{
  string::size_type old = x_spam_checker_version.size();
  x_spam_checker_version = val;
  return (old);
}

string::size_type
SpamAssassin::set_spam_level(const string& val)
{
  string::size_type old = x_spam_level.size();
  x_spam_level = val;
  return (old);
}

string::size_type
SpamAssassin::set_content_type(const string& val)
{
  string::size_type old = _content_type.size();
  _content_type = val;
  return (old);
}

string::size_type
SpamAssassin::set_subject(const string& val)
{
  string::size_type old = _subject.size();
  _subject = val;
  return (old);
}

string::size_type
SpamAssassin::set_rcpt(const string& val)
{
  string::size_type old = _rcpt.size();
  _rcpt = val;
  return (old);  
}

string::size_type
SpamAssassin::set_from(const string& val)
{
  string::size_type old = _from.size();
  _from = val;
  return (old);  
}

string::size_type
SpamAssassin::set_connectip(const string& val)
{
  string::size_type old = _connectip.size();
  _connectip = val;
  return (old);  
}

//
// Read available output from SpamAssassin client
//
int
SpamAssassin::read_pipe()
{
	long size;
	int  status;
	char iobuff[1024];
	string reason;

	debug(D_FUNC, "::read_pipe enter");

	if (pipe_io[1][0] == -1)
	{
		debug(D_FUNC, "::read_pipe exit - shouldn't have been called?");
		return 0;
	}

	size = read(pipe_io[1][0], iobuff, 1024);

	if (size < 0)
    {
		// Error. 
		reason = string(strerror(errno));
		
		// Close remaining pipe.
		close(pipe_io[1][0]);
		pipe_io[1][0] = -1;
	
		// Slaughter child
		kill(pid, SIGKILL);
	
		// set flags
		error = true;
		running = false;
	
		// wait until child is dead
		waitpid(pid, &status, 0);
	
		// throw the error message that caused this trouble
		throw string(string("read error: ")+reason);
	} else if ( size == 0 )
	{

		// EOF. Close the pipe
		if(close(pipe_io[1][0]))
			throw string(string("close error: ")+string(strerror(errno)));
		pipe_io[1][0] = -1;
	
	} else
	{ 
		// append to mail buffer 
		mail.append(iobuff, size);
		debug(D_POLL, "read %ld bytes", size);
		debug(D_SPAMC, "input  \"%*.*s\"", (int)size, (int)size, iobuff);
	}
	debug(D_FUNC, "::read_pipe exit");
	return size;
}

//
// Read all output from SpamAssassin client
// and close the pipe
//
void
SpamAssassin::empty_and_close_pipe()
{
	debug(D_FUNC, "::empty_and_close_pipe enter");
	while (read_pipe())
		;
	debug(D_FUNC, "::empty_and_close_pipe exit");
}

// }}}

// {{{ Some small subroutines without much relation to functionality

// output error message to syslog facility
void
throw_error(const string& errmsg)
{
  if (errmsg.c_str())
    syslog(LOG_ERR, errmsg.c_str());
  else
    syslog(LOG_ERR, "Unknown error");
}

/* Takes a comma or space-delimited string of debug tokens and sets the
   appropriate bits in flag_debug.  "all" sets all the bits.
*/
void parse_debuglevel(char* string)
{
	char *token;

	/* handle the old numeric values too */
	switch(atoi(string))
	{
		case 3:
			flag_debug |= (1<<D_UORI) | (1<<D_STR);
		case 2:
			flag_debug |= (1<<D_POLL);
		case 1:
			flag_debug |= (1<<D_MISC) | (1<<D_FUNC);
			debug(D_ALWAYS, "Setting debug level to 0x%0x", flag_debug);
			return;
		default:
			break;
	}

	while ((token = strsep(&string, ", ")))
	{
		int i;
		for (i=0; debugstrings[i]; i++)
		{
			if(strcasecmp(token, "ALL")==0)
			{
				flag_debug = (1<<D_MAX)-1;
				break;
			}
			if(strcasecmp(token, debugstrings[i])==0)
			{
				flag_debug |= (1<<i);
				break;
			}
		}

		if (!debugstrings[i])
		{
			fprintf(stderr, "Invalid debug token \"%s\"\n", token);
			exit(1);
		}
	}
	debug(D_ALWAYS, "Setting debug level to 0x%0x", flag_debug);
}

/*
   Output a line to syslog using print format, but only if the appropriate
   debug level is set.  The D_ALWAYS level is always enabled.
*/
void debug(enum debuglevel level, const char* string, ...)
{
	if ((1<<level) & flag_debug)
	{
#if defined(HAVE_VSYSLOG)
	    va_list vl;
	    va_start(vl, string);
		vsyslog(LOG_ERR, string, vl);
		va_end(vl);
#else
#if defined(HAVE_VASPRINTF)
		char *buf;
#else
		char buf[1024];
#endif
	    va_list vl;
	    va_start(vl, string);
#if defined(HAVE_VASPRINTF)
	    vasprintf(&buf, string, vl);
#else
#if defined(HAVE_VSNPRINTF)
	    vsnprintf(buf, sizeof(buf)-1, string, vl);
#else
		/* XXX possible buffer overflow here; be careful what you pass to debug() */
		vsprintf(buf, string, vl);
#endif
#endif
	    va_end(vl);
		syslog(LOG_ERR, "%s", buf);
#if defined(HAVE_VASPRINTF)
	    free(buf);
#endif 
#endif /* vsyslog */
	}
}

// case-insensitive search 
string::size_type 
find_nocase(const string& array, const string& pattern, string::size_type start)
{
  string::size_type pos(start);

  while (pos < array.size())
    {
      string::size_type ctr(0);

      while( (pos+ctr) < array.size() &&
	     toupper(array[pos+ctr]) == toupper(pattern[ctr]) )
	{
	  ++ctr;
	  if (ctr == pattern.size())
	  {
	    debug(D_STR, "f_nc: <%s><%s>: hit", array.c_str(), pattern.c_str());
	    return pos;
	  }
	};
      
      ++pos;
    };

  debug(D_STR, "f_nc: <%s><%s>: nohit", array.c_str(), pattern.c_str());
  return string::npos;
}

// compare case-insensitive
int
cmp_nocase_partial(const string& s, const string& s2)
{
  string::const_iterator p=s.begin();
  string::const_iterator p2=s2.begin();

  while ( p != s.end() && p2 != s2.end() ) {
    if (toupper(*p) != toupper(*p2))
    {
      debug(D_STR, "c_nc_p: <%s><%s> : miss", s.c_str(), s2.c_str());
      return (toupper(*p) < toupper(*p2)) ? -1 : 1;
    }
    ++p;
    ++p2;
  };

  debug(D_STR, "c_nc_p: <%s><%s> : hit", s.c_str(), s2.c_str());
  return 0;

}

/* closeall() - close all FDs >= a specified value */ 
void closeall(int fd) 
{
	int fdlimit = sysconf(_SC_OPEN_MAX); 
	while (fd < fdlimit) 
		close(fd++); 
}

void parse_networklist(char *string, struct networklist *list)
{
	char *token;

	while ((token = strsep(&string, ", ")))
	{
		char *tnet = strsep(&token, "/");
		char *tmask = token;
		struct in_addr net, mask;

		if (list->num_nets % 10 == 0)
			list->nets = (struct net*)realloc(list->nets, sizeof(*list->nets) * (list->num_nets + 10));

		if (!inet_aton(tnet, &net))
		{
			fprintf(stderr, "Could not parse \"%s\" as a network\n", tnet);
			exit(1);
		}

		if (tmask)
		{
			if (strchr(tmask, '.') == NULL)
			{
				/* CIDR */
				unsigned int bits;
				int ret;
				ret = sscanf(tmask, "%u", &bits);
				if (ret != 1 || bits > 32)
				{
					fprintf(stderr,"%s: bad CIDR value", tmask);
					exit(1);
				}
				mask.s_addr = htonl(~((1L << (32 - bits)) - 1) & 0xffffffff);
			} else if (!inet_aton(tmask, &mask))
			{
				fprintf(stderr, "Could not parse \"%s\" as a netmask\n", tmask);
				exit(1);
			}
		} else
			mask.s_addr = 0xffffffff;

		{
			char *snet = strdup(inet_ntoa(net));
			debug(D_MISC, "Adding %s/%s to network list", snet, inet_ntoa(mask));
			free(snet);
		}

		net.s_addr = net.s_addr & mask.s_addr;
		list->nets[list->num_nets].network = net;
		list->nets[list->num_nets].netmask = mask;
		list->num_nets++;
	}
}

int ip_in_networklist(struct in_addr ip, struct networklist *list)
{
	int i;

	debug(D_NET, "Checking %s against:", inet_ntoa(ip));
	for (i = 0; i < list->num_nets; i++)
	{
		debug(D_NET, "%s", inet_ntoa(list->nets[i].network));
		debug(D_NET, "/%s", inet_ntoa(list->nets[i].netmask));
		if ((ip.s_addr & list->nets[i].netmask.s_addr) == list->nets[i].network.s_addr)
        {
        	debug(D_NET, "Hit!");
			return 1;
		}
	}

	return 0;
}


// }}}
// vim6:ai:noexpandtab
