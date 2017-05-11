[![Build Status](https://travis-ci.org/ZenProjects/Apache-Status-Text-Module.svg?branch=master)](https://travis-ci.org/ZenProjects/Apache-Status-Text-Module)
[![CircleCI](https://circleci.com/gh/ZenProjects/Apache-Status-Text-Module.svg?style=svg)](https://circleci.com/gh/ZenProjects/Apache-Status-Text-Module)
[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3-blue.svg)](http://www.gnu.org/licenses/gpl-3.0)

Apache Status Text Module
=========================

# What is it?

   This is a full text fork of mod_status for only machine automatique parsing.

   This is Status Module.  They display lots of internal data about how Apache is
   performing and the state of all children processes.

# How to build ? #

```
# unzip <module archive>.zip
# cd <module directory>
# ./configure --with-apxs=/path/to/apxs
# make
# make install
```

# How to use it ?

   Load mod_status_text and to enable it, add the following lines into any config file:

```
   LoadModule status_text_module modules/mod_status_text.so
   <Location /server-status-text>
   SetHandler server-status-text
   </Location>
```

   You may want to protect this location by password or domain so no one
   else can look at it.  Then you can access the statistics with a URL like:

```
   http://your_server_name/server-status-text
```

   They returns full text server status

```
   http://your_server_name/server-status-text?<statistics_key>
```

   They return the value of the "statistics_key" only.
   
   Also count number of requetes per seconds, 95% percentil and per type of response (5xx,4xx,3xx,2xx,1xx, etc...).
   
