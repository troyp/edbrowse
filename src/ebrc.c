/* url.c
 * Sample .ebrc file.
 * This file is part of the edbrowse project, released under GPL.
 */

const char *pebrc = "#  .ebrc: sample configuration file for edbrowse (version 3.4.8 or above)\n"
"#  email account; you may have several.\n"
"#  a gmail account, most people have one of these now adays.\n"
"mail {\n"
"inserver = pop.gmail.com\n"
"outserver = smtp.gmail.com\n"
"secure\n"
"login = edbrowse\n"
"password = rhinoceros\n"
"from = Donald Duck\n"
"reply = edbrowse@gmail.com\n"
"}\n"
"\n"
"#  Add address book.\n"
"#adbook=/home/mylogin/outside/adbook\n"
"\n"
"#  inbox. Should be an absolute path.\n"
"#maildir = /home/mylogin/mbox\n"
"\n"
"#  Place downloaded files here. Should be an absolute path.\n"
"# downdir = /home/mylogin/downloads\n"
"\n"
"#  The cookie jar - where we store the http cookies.\n"
"#jar = /home/mylogin/outside/cookies\n"
"\n"
"#  file of ssl certificates\n"
"# certfile = /etc/ssl/cert.pem\n"
"\n"
"#  wait 30 seconds for a response from a web server\n"
"webtimer = 30\n"
"#  wait 3 minutes for a response from a mail server\n"
"mailtimer = 180\n"
"\n"
"#  Redirect mail based on the sender, or the destination account.\n"
"fromfilter {\n"
"fred flintstone > fredmail\n"
"fred.flintstone@bedrock.us > fredmail\n"
"jerk@hotmail.com > x\n"
"word@m-w.com > -wod\n"
"}\n"
"\n"
"# tofilter { }\n"
"\n"
"#  Describe the mime types and the plugins to run them.\n"
"plugin {\n"
"type = audio/basic\n"
"desc = audio file in a wave format\n"
"suffix = wav,voc,au,ogg\n"
"content = audio/x-wav\n"
"#  %i is the temp input file generated by edbrowse\n"
"program = start %i\n"
"}\n"
"\n"
"#  Every time you fetch a web page from the internet,\n"
"#  your browser identifies itself to the host.\n"
"agent = Lynx/2.8.4rel.1 libwww-FM/2.14\n"
"agent = Mozilla/4.0 (compatible; MSIE 5.5; Windows 98; Win 9x 4.90)\n"
"\n"
"#  Ok, we're ready to write our first script.\n"
"#  How about a function to access google.\n"
"#  So   <gg elephants tigers   will call up google,\n"
"#  looking for elephants and tigers together.\n"
"function+gg {\n"
"b http://www.google.com\n"
"/<>/ i=~0\n"
"+i1*\n"
"/^1/+\n"
"}\n"
"\n"
"#  mariam-webster dictionary lookup, ~1 is parameter 1, the word to look up.\n"
"#  <mw elephant\n"
"function+mw {\n"
"b http://www.merriam-webster.com/dictionary/~1\n"
"}\n"
"# and much more...\n";

// eof - ebrc.c
