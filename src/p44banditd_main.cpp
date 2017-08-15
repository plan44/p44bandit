//
//  Copyright (c) 2016 plan44.ch / Lukas Zeller, Zurich, Switzerland
//
//  Author: Lukas Zeller <luz@plan44.ch>
//
//  This file is part of p44banditd.
//
//  pixelboardd is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  pixelboardd is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with pixelboardd. If not, see <http://www.gnu.org/licenses/>.
//

#include "application.hpp"

#include "jsoncomm.hpp"

#include "banditcomm.hpp"

#include <dirent.h>

using namespace p44;

#define MAINLOOP_CYCLE_TIME_uS 10000 // 10mS
#define DEFAULT_LOGLEVEL LOG_NOTICE


// MARK: ==== Application

typedef boost::function<void (JsonObjectPtr aResponse, ErrorPtr aError)> RequestDoneCB;

class P44BanditD : public CmdLineApp
{
  typedef CmdLineApp inherited;

  // API Server
  SocketCommPtr apiServer;
  BanditCommPtr banditComm;

  MLMicroSeconds starttime;

  // data dir
  string datadir;
  string selectedfile;

public:

  P44BanditD() :
    starttime(MainLoop::now())
  {
  }

  virtual int main(int argc, char **argv)
  {
    const char *usageText =
      "Usage: %1$s [options]\n";
    const CmdLineOptionDescriptor options[] = {
      { 0  , "jsonapiport",    true,  "port;server port number for JSON API (default=none)" },
      { 0  , "jsonapinonlocal",false, "allow JSON API from non-local clients" },
      { 'l', "loglevel",       true,  "level;set max level of log message detail to show on stdout" },
      { 0  , "errlevel",       true,  "level;set max level for log messages to go to stderr as well" },
      { 0  , "dontlogerrors",  false, "don't duplicate error messages (see --errlevel) on stdout" },
      { 0  , "serialport",     true,  "serial port device; specify the serial port device" },
      { 0  , "hsoutpin",       true,  "pin specification; serial handshake output line" },
      { 0  , "hsinpin",        true,  "pin specification; serial handshake input line" },
      { 0  , "datadir",        true,  "filepath;where to save machine data files" },

      // temp & experimental
      { 0  , "receive",        false, "receive data from bandit and show it on stdout" },
      { 0  , "startonhs",      false, "start only on input handshake becoming active" },
      { 0  , "stoponhs",       false, "stop only on input handshake becoming inactive" },
      { 0  , "hsonstart",      false, "set handshake line active already before sending or receiving" },
      { 0  , "send",           true,  "file; send file to bandit" },
      { 'h', "help",           false, "show this text" },
      { 0, NULL } // list terminator
    };

    // parse the command line, exits when syntax errors occur
    setCommandDescriptors(usageText, options);
    parseCommandLine(argc, argv);

    if ((numOptions()<1) || numArguments()>0) {
      // show usage
      showUsage();
      terminateApp(EXIT_SUCCESS);
    }

    // build objects only if not terminated early
    if (!isTerminated()) {
      int loglevel = DEFAULT_LOGLEVEL;
      getIntOption("loglevel", loglevel);
      SETLOGLEVEL(loglevel);
      int errlevel = LOG_ERR; // testing by default only reports to stdout
      getIntOption("errlevel", errlevel);
      SETERRLEVEL(errlevel, !getOption("dontlogerrors"));

      // - create and start bandit comm
      banditComm = BanditCommPtr(new BanditComm(MainLoop::currentMainLoop()));
      string serialport;
      if (getStringOption("serialport", serialport)) {
        banditComm->setConnectionSpecification(serialport.c_str(), 2101, getOption("hsoutpin", "missing"), getOption("hsinpin", "missing"));
      }

      // - find the data directory
      datadir = "/tmp/data";
      if (getStringOption("datadir", datadir)) {
        pathstring_format_append(datadir, "");
      }

      // - create and start API server and wait for things to happen
      string apiport;
      if (getStringOption("jsonapiport", apiport)) {
        apiServer = SocketCommPtr(new SocketComm(MainLoop::currentMainLoop()));
        apiServer->setConnectionParams(NULL, apiport.c_str(), SOCK_STREAM, AF_INET);
        apiServer->setAllowNonlocalConnections(getOption("jsonapinonlocal"));
        apiServer->startServer(boost::bind(&P44BanditD::apiConnectionHandler, this, _1), 3);
      }


    } // if !terminated
    // app now ready to run (or cleanup when already terminated)
    return run();
  }


//  bool string_load(FILE *aFile, string &aData)
//  {
//    const size_t bufLen = 1024;
//    char buf[bufLen];
//    aData.clear();
//    bool eof = false;
//    while (!eof) {
//      char *p = fgets(buf, bufLen-1, aFile);
//      if (!p) {
//        // eof or error
//        if (feof(aFile)) return !aData.empty(); // eof is ok if it occurs after having collected some data, otherwise it means: no more lines
//        return false;
//      }
//      // something read
//      size_t l = strlen(buf);
//      // check for CR, LF or CRLF
//      if (l>0 && buf[l-1]=='\n') {
//        l--;
//        eol = true;
//      }
//      if (l>0 && buf[l-1]=='\r') {
//        l--;
//        eol = true;
//      }
//      // collect
//      aLine.append(buf,l);
//    }
//    return true;
//  }



//  string data =
//    "N001&G99\n"
//    "Z18.Y111.X-217.G92\n"
//    "G98\n"
//    "G91\n"
//    "F80.\n"
//    "X-4.\n"
//    "I4.\n"
//    "/G5\n"
//    "N8\n";


  virtual void initialize()
  {
    banditComm->init(); // idle
    string fn;
    if (getOption("receive")) {
      banditComm->receive(
        boost::bind(&P44BanditD::receiveResult, this, _1, _2),
        getOption("hsonstart"),
        getOption("startonhs"),
        getOption("stoponhs")
      );
    }
    else if (getStringOption("send", fn)) {
      string data;
      FILE *inFile = fopen(fn.c_str(), "r");
      if (inFile && string_fgetfile(inFile, data)) {
        banditComm->send(
          boost::bind(&P44BanditD::sendComplete, this, _1),
          data,
          getOption("hsonstart")
        );
      }
      else {
        LOG(LOG_ERR, "Cannot open input file '%s'", fn.c_str());
        terminateApp(1);
      }
    }
    else {
      // Normal operation:
      LOG(LOG_NOTICE, "Start receiving automatically when handshake line indicates data");
      autoReceive();
    }
  }


  void sendComplete(ErrorPtr aError)
  {
    if (Error::isOK(aError)) {
      // print data to stdout
      LOG(LOG_NOTICE, "Successfully sent data");
    }
    else {
      LOG(LOG_ERR, "Error receiving data: %s", aError->description().c_str());
    }
    terminateAppWith(aError);
  }



  void receiveResult(const string &aResponse, ErrorPtr aError)
  {
    if (Error::isOK(aError)) {
      // print data to stdout
      LOG(LOG_NOTICE, "Successfully received %zd bytes of data", aResponse.size());
      fputs(aResponse.c_str(), stdout);
      fflush(stdout);
    }
    else {
      LOG(LOG_ERR, "Error receiving data: %s", aError->description().c_str());
    }
    terminateAppWith(aError);
  }


  // MARK: ==== Normal operation
  void autoReceive()
  {
    LOG(LOG_INFO, "Start waiting for new data...");
    banditComm->receive(
      boost::bind(&P44BanditD::autoReceived, this, _1, _2),
      true, // hsonstart
      true, // startonhs
      true // stoponhs
    );
  }


  void autoReceived(const string &aResponse, ErrorPtr aError)
  {
    if (Error::isOK(aError)) {
      // print data to stdout
      size_t receivedBytes = aResponse.size();
      LOG(LOG_INFO, "Successfully received %zd bytes of data", receivedBytes);
      if (aResponse.size()>0) {
        string ts = string_ftime("%Y-%m-%d_%H.%M.%S", NULL);
        string fp = datadir;
        pathstring_format_append(fp, "%s_bandit_transmit", ts.c_str());
        LOG(LOG_NOTICE, "Saving received data (%zd bytes) to '%s'", receivedBytes, fp.c_str());
        FILE *datafileP = fopen(fp.c_str(), "w");
        if (datafileP==NULL) {

        }
        else {
          // save data
          if (fwrite(aResponse.c_str(), 1, receivedBytes, datafileP)<receivedBytes) {
            LOG(LOG_ERR, "Cannot save received file %s - %s", fp.c_str(), strerror(errno));
          }
          // close file
          fclose(datafileP);
        }
      }
    }
    else {
      LOG(LOG_ERR, "Error auto-receiving data: %s", aError->description().c_str());
    }
    // restart receiving (with a small safety delay)
    MainLoop::currentMainLoop().executeOnce(boost::bind(&P44BanditD::autoReceive, this), 1*Second);
  }




  // MARK: ==== API access


  SocketCommPtr apiConnectionHandler(SocketCommPtr aServerSocketComm)
  {
    JsonCommPtr conn = JsonCommPtr(new JsonComm(MainLoop::currentMainLoop()));
    conn->setMessageHandler(boost::bind(&P44BanditD::apiRequestHandler, this, conn, _1, _2));
    conn->setClearHandlersAtClose(); // close must break retain cycles so this object won't cause a mem leak
    return conn;
  }


  void apiRequestHandler(JsonCommPtr aConnection, ErrorPtr aError, JsonObjectPtr aRequest)
  {
    // Decode mg44-style request (HTTP wrapped in JSON)
    if (Error::isOK(aError)) {
      LOG(LOG_INFO,"API request: %s", aRequest->c_strValue());
      JsonObjectPtr o;
      o = aRequest->get("method");
      if (o) {
        string method = o->stringValue();
        string uri;
        o = aRequest->get("uri");
        if (o) uri = o->stringValue();
        JsonObjectPtr data;
        bool upload = false;
        bool action = (method!="GET");
        // check for uploads
        string uploadedfile;
        if (aRequest->get("uploadedfile", o)) {
          uploadedfile = o->stringValue();
          upload = true;
          action = false; // other params are in the URI, not the POSTed upload
        }
        if (action) {
          // JSON data is in the request
          data = aRequest->get("data");
        }
        else {
          // URI params is the JSON to process
          data = aRequest->get("uri_params");
          if (data) action = true; // GET, but with query_params: treat like PUT/POST with data
          if (upload) {
            // move that into the request
            data->add("uploadedfile", JsonObject::newString(uploadedfile));
          }
        }
        // request elements now: uri and data
        if (processRequest(uri, data, action, boost::bind(&P44BanditD::requestHandled, this, aConnection, _1, _2))) {
          // done, callback will send response and close connection
          return;
        }
        // request cannot be processed, return error
        LOG(LOG_ERR,"Invalid JSON request");
        aError = WebError::webErr(404, "No handler found for request to %s", uri.c_str());
      }
      else {
        LOG(LOG_ERR,"Invalid JSON request");
        aError = WebError::webErr(415, "Invalid JSON request format");
      }
    }
    // return error
    requestHandled(aConnection, JsonObjectPtr(), aError);
  }


  void requestHandled(JsonCommPtr aConnection, JsonObjectPtr aResponse, ErrorPtr aError)
  {
    if (!aResponse) {
      aResponse = JsonObject::newObj(); // empty response
    }
    if (!Error::isOK(aError)) {
      aResponse->add("Error", JsonObject::newString(aError->description()));
    }
    LOG(LOG_INFO,"API answer: %s", aResponse->c_strValue());
    aConnection->sendMessage(aResponse);
    aConnection->closeAfterSend();
  }


  bool processRequest(string aUri, JsonObjectPtr aData, bool aIsAction, RequestDoneCB aRequestDoneCB)
  {
    ErrorPtr err;
    JsonObjectPtr o;
    if (aUri=="files") {
      // return a list of files
      string action;
      if (!aData->get("action", o)) {
        aIsAction = false;
      }
      else {
        action = o->stringValue();
      }
      if (!aIsAction) {
        DIR *dirP = opendir (datadir.c_str());
        struct dirent *direntP;
        if (dirP==NULL) {
          err = SysError::errNo("Cannot read data directory");
        }
        else {
          JsonObjectPtr files = JsonObject::newArray();
          bool foundSelected = false;
          while ((direntP = readdir(dirP))!=NULL) {
            string fn = direntP->d_name;
            if (fn=="." || fn=="..") continue;
            JsonObjectPtr file = JsonObject::newObj();
            file->add("name", JsonObject::newString(fn));
            file->add("ino", JsonObject::newInt64(direntP->d_ino));
            file->add("type", JsonObject::newInt64(direntP->d_type));
            if (fn==selectedfile) foundSelected = true;
            file->add("selected", JsonObject::newBool(fn==selectedfile));
            files->arrayAppend(file);
          }
          closedir (dirP);
          if (!foundSelected) selectedfile.clear(); // remove selection not matching any of the existing files
          aRequestDoneCB(files, ErrorPtr());
          return true;
        }
      }
      else {
        // a file action
        if (!aData->get("name", o)) {
          err = WebError::webErr(400, "Missing 'name'");
        }
        else {
          // addressing a particular file
          // - try to open to see if it exists
          string filename = o->c_strValue();
          string filepath = datadir;
          pathstring_format_append(filepath, "%s", filename.c_str());
          FILE *fileP = fopen(filepath.c_str(),"r");
          if (fileP==NULL) {
            err = WebError::webErr(404, "File '%s' not found", filename.c_str());
          }
          else {
            // exists
            fclose(fileP);
            if (action=="rename") {
              // rename file
              if (!aData->get("newname", o)) {
                err = WebError::webErr(400, "Missing 'newname'");
              }
              else {
                string newname = o->stringValue();
                if (newname.size()<3) {
                  err = WebError::webErr(415, "'newname' is too short (min 3 characters)");
                }
                else {
                  string newpath = datadir;
                  pathstring_format_append(newpath, "%s", newname.c_str());
                  if (rename(filepath.c_str(), newpath.c_str())!=0) {
                    err = SysError::errNo("Cannot rename file");
                  }
                }
              }
            }
//              else if (action=="download") {
//
//              }
            else if (action=="delete") {
              if (unlink(filepath.c_str())!=0) {
                err = SysError::errNo("Cannot delete file");
              }
            }
            else if (action=="select") {
              if (selectedfile==filename) {
                selectedfile.clear(); // unselect
              }
              else {
                selectedfile = filename; // select
              }
            }
            else {
              err = WebError::webErr(400, "Unknown files action");
            }
          }
        }
      }
      actionStatus(aRequestDoneCB, err);
      return true;
    }
    else if (aIsAction && aUri=="log") {
      if (aData->get("level", o)) {
        int lvl = o->int32Value();
        LOG(LOG_NOTICE, "\n====== Changed Log Level from %d to %d\n", LOGLEVEL, lvl);
        SETLOGLEVEL(lvl);
      }
      actionStatus(aRequestDoneCB, err);
      return true;
    }

//    else if (aUri=="player1" || aUri=="player2") {
//      int side = aUri=="player2" ? 1 : 0;
//      if (aIsAction) {
//        if (aData->get("key", o)) {
//          string key = o->stringValue();
//          // Note: assume keys are already released when event is reported
//          if (key=="left")
//            keyHandler(side, keycode_left, keycode_none);
//          else if (key=="right")
//            keyHandler(side, keycode_right, keycode_none);
//          else if (key=="turn")
//            keyHandler(side, keycode_middleleft, keycode_none);
//          else if (key=="drop")
//            keyHandler(side, keycode_middleright, keycode_none);
//        }
//      }
//      aRequestDoneCB(JsonObjectPtr(), ErrorPtr());
//      return true;
//    }
//    else if (aUri=="board") {
//      if (aIsAction) {
//        PageMode mode = pagemode_controls1; // default to bottom controls
//        if (aData->get("mode", o))
//          mode = o->int32Value();
//        if (aData->get("page", o)) {
//          string page = o->stringValue();
//          gotoPage(page, mode);
//        }
//      }
//      aRequestDoneCB(JsonObjectPtr(), ErrorPtr());
//      return true;
//    }
//    else if (aUri=="page") {
//      // ask each page
//      for (PagesMap::iterator pos = pages.begin(); pos!=pages.end(); ++pos) {
//        if (pos->second->handleRequest(aData, aRequestDoneCB)) {
//          // request will be handled by this page, done for now
//          return true;
//        }
//      }
//    }
    else if (aUri=="/") {
      string uploadedfile;
      string cmd;
      if (aData->get("uploadedfile", o))
        uploadedfile = o->stringValue();
      if (aData->get("cmd", o))
        cmd = o->stringValue();
//      if (cmd=="imageupload" && displayPage) {
//        ErrorPtr err = displayPage->loadPNGBackground(uploadedfile);
//        gotoPage("display", false);
//        updateDisplay();
//        aRequestDoneCB(JsonObjectPtr(), err);
//        return true;
//      }
    }
    return false;
  }


  void actionDone(RequestDoneCB aRequestDoneCB)
  {
    aRequestDoneCB(JsonObjectPtr(), ErrorPtr());
  }


  void actionStatus(RequestDoneCB aRequestDoneCB, ErrorPtr aError = ErrorPtr())
  {
    aRequestDoneCB(JsonObjectPtr(), aError);
  }
  


  ErrorPtr processUpload(string aUri, JsonObjectPtr aData, const string aUploadedFile)
  {
    ErrorPtr err;

    string cmd;
    JsonObjectPtr o;
    if (aData->get("cmd", o)) {
      cmd = o->stringValue();
//      if (cmd=="imageupload") {
//        displayPage->loadPNGBackground(aUploadedFile);
//        gotoPage("display", false);
//        updateDisplay();
//      }
//      else
      {
        err = WebError::webErr(500, "Unknown upload cmd '%s'", cmd.c_str());
      }
    }
    return err;
  }



};





int main(int argc, char **argv)
{
  // prevent debug output before application.main scans command line
  SETLOGLEVEL(LOG_EMERG);
  SETERRLEVEL(LOG_EMERG, false); // messages, if any, go to stderr
  // create the mainloop
  MainLoop::currentMainLoop().setLoopCycleTime(MAINLOOP_CYCLE_TIME_uS);
  // create app with current mainloop
  static P44BanditD application;
  // pass control
  return application.main(argc, argv);
}
