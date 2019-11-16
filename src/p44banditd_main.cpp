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
#include <sys/stat.h> // for fstat

using namespace p44;

#define MAINLOOP_CYCLE_TIME_uS 10000 // 10mS
#define DEFAULT_LOGLEVEL LOG_NOTICE


// MARK: ==== Application

typedef boost::function<void (JsonObjectPtr aResponse, ErrorPtr aError)> RequestDoneCB;


#define MAX_COPYFILE_BUF_SIZE 20000

static ErrorPtr copyfile(const string aSourcePath, const string aDestPath)
{
  size_t bufSize = MAX_COPYFILE_BUF_SIZE;
  int srcfd = open(aSourcePath.c_str(), O_RDONLY);
  if (srcfd<0) {
    return SysError::errNo(string_format("copyfile: cannot open input file '%s'", aSourcePath.c_str()).c_str());
  }
  // opened, check buffer needs
  struct stat fs;
  fstat(srcfd, &fs);
  if (fs.st_size<bufSize) bufSize = fs.st_size; // don't need the entire buffer
  // open destination file
  int destfd = open(aDestPath.c_str(), O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR);
  if (destfd<0) {
    close(srcfd);
    return SysError::errNo(string_format("copyfile: cannot open output file '%s'", aDestPath.c_str()).c_str());
  }
  // copy
  char *buffer = new char[bufSize];
  ssize_t n;
  while((n = read(srcfd, buffer, bufSize))>0) {
    n = write(destfd, buffer, n);
    if (n<0) break;
  }
  close(destfd);
  close(srcfd);
  delete[] buffer;
  if (n<0) {
    return SysError::errNo("copyfile: error copying data");
  }
  return ErrorPtr();
}


static string cleanBanditData(const string aData, bool aForSend, bool aRawMode)
{
  if (aRawMode) return aData; // pass trough
  string res;
  char lastChar = 0;
  // skip all trailing control chars and spaces first
  size_t i=0;
  while (i<aData.size()) {
    if (aData[i]>0x20) break;
    i++;
  }
  for (;i<aData.size(); ++i) {
    char c = aData[i];
    if (c=='\n' || c=='\r') {
      // newline
      if (lastChar=='\n') {
        continue; // no duplicates
      }
      c = '\n';
      if (aForSend) res += '\r'; // output with CR+LF
    }
    else if (c<0x20 || c>0x7E) {
      continue; // filter all control chars (DC1 0x11 at beginning, many nulls, DC4 0x13 at end)
    }
    res += c;
    lastChar = c;
  }
  return res;
}


class P44BanditD : public CmdLineApp
{
  typedef CmdLineApp inherited;

  // API Server
  SocketCommPtr apiServer;

  // BANDIT communication
  BanditCommPtr banditComm;
  bool rawmode;

  // LED+Button
  ButtonInputPtr button;
  IndicatorOutputPtr greenLed;
  IndicatorOutputPtr redLed;

  MLMicroSeconds starttime;
  MLTicket autoReceiveTicket;
  

  // data dir
  string selectedfile;

public:

  P44BanditD() :
    starttime(MainLoop::now()),
    rawmode(false)
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
      { 0  , "deltatstamps",  false, "show timestamp delta between log lines" },
      { 0  , "serialport",     true,  "serial port device; specify the serial port device" },
      { 0  , "hsoutpin",       true,  "pin specification; serial handshake output line" },
      { 0  , "hsinpin",        true,  "pin specification; serial handshake input line" },
      { 0  , "button",         true,  "input pinspec; device button" },
      { 0  , "greenled",       true,  "output pinspec; green device LED" },
      { 0  , "redled",         true,  "output pinspec; red device LED" },
      CMDLINE_APPLICATION_STDOPTIONS,
      CMDLINE_APPLICATION_PATHOPTIONS,

      // temp & experimental
      { 0  , "receive",        false, "receive data from bandit and show it on stdout" },
      { 0  , "startonhs",      false, "start only on input handshake becoming active" },
      { 0  , "stoponhs",       false, "stop only on input handshake becoming inactive" },
      { 0  , "hsonstart",      false, "set handshake line active already before sending or receiving" },
      { 0  , "rawmode",        false, "send/receive raw data to/from Bandit" },
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
      SETDELTATIME(getOption("deltatstamps"));

      // create button input
      button = ButtonInputPtr(new ButtonInput(getOption("button","missing")));
      button->setButtonHandler(boost::bind(&P44BanditD::buttonHandler, this, _1, _2, _3), true, Second);
      // - create LEDs
      greenLed = IndicatorOutputPtr(new IndicatorOutput(getOption("greenled","missing")));
      redLed = IndicatorOutputPtr(new IndicatorOutput(getOption("redled","missing")));

      // - create and start bandit comm
      banditComm = BanditCommPtr(new BanditComm(MainLoop::currentMainLoop()));
      string serialport;
      if (getStringOption("serialport", serialport)) {
        banditComm->setConnectionSpecification(serialport.c_str(), 2101, getOption("hsoutpin", "missing"), getOption("hsinpin", "missing"));
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


  virtual void initialize()
  {
    banditComm->init(); // idle
    string fn;
    rawmode = getOption("rawmode");
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
      LOG(LOG_ERR, "Error sending data: %s", aError->description().c_str());
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
      redLed->onFor(2*Second);
      if (aResponse.size()>0) {
        string ts = string_ftime("%Y-%m-%d_%H.%M.%S", NULL);
        string fp = Application::sharedApplication()->dataPath(string_format("%s_bandit_download.txt", ts.c_str()));
        LOG(LOG_NOTICE, "Saving received data (%zd bytes) to '%s'", receivedBytes, fp.c_str());
        FILE *datafileP = fopen(fp.c_str(), "w");
        if (datafileP==NULL) {
          LOG(LOG_ERR, "Error opening file for write: %s", strerror(errno));
        }
        else {
          // clean data
          string data = cleanBanditData(aResponse, false, rawmode);
          size_t dataSize = data.size();
          // save data
          if (fwrite(data.c_str(), 1, dataSize, datafileP)<dataSize) {
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
    autoReceiveTicket.executeOnce(boost::bind(&P44BanditD::autoReceive, this), 1*Second);
  }


  ErrorPtr sendFile(const string aFilePath)
  {
    ErrorPtr err;
    string data;
    FILE *inFile = fopen(aFilePath.c_str(), "r");
    if (inFile==NULL || !string_fgetfile(inFile, data)) {
      return SysError::errNo("cannot open file to send: ");
    }
    else {
      // clean data
      string senddata = "\x11"; // always DC1/XON at beginning
      senddata.append(100, 0); // 100 null chars padding
      senddata += "\r"; // single CR in front of first line
      senddata += cleanBanditData(data, true, rawmode); // data itself, with double LFs
      // make sure data ends with CR LF
      if (senddata[senddata.size()-1]!='\n') {
        senddata += "\r\n";
      }
      senddata += "\x13"; // always DC3/XOFF character at the end of file
      senddata.append(100, 0); // 100 null chars padding
      LOG(LOG_NOTICE, "Sending data (%lu bytes input data, %lu bytes padded+cleaned) from '%s'", data.size(), senddata.size(), aFilePath.c_str());
      // send it
      redLed->steadyOn();
      banditComm->send(
        boost::bind(&P44BanditD::sendFileComplete, this, _1),
        senddata,
        true // hsonstart
      );
    }
    return err;
  }



  void sendFileComplete(ErrorPtr aError)
  {
    redLed->steadyOff();
    if (Error::isOK(aError)) {
      // print data to stdout
      LOG(LOG_NOTICE, "Successfully sent data");
    }
    else {
      LOG(LOG_ERR, "Error sending data: %s", aError->description().c_str());
    }
  }



  // MARK: ==== Button


  void buttonHandler(bool aState, bool aHasChanged, MLMicroSeconds aTimeSincePreviousChange)
  {
    LOG(LOG_INFO, "Button state now %d%s", aState, aHasChanged ? " (changed)" : " (same)");
    if (aHasChanged && !aState && selectedfile.size()>0) {
      // send the selected file
      string filepath = Application::sharedApplication()->dataPath(selectedfile.c_str());
      ErrorPtr err = sendFile(filepath);
      if (!Error::isOK(err)) {
        LOG(LOG_ERR, "Cannot send file: %s", err->description().c_str());
      }
    }
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


  ErrorPtr processUpload(string aUri, JsonObjectPtr aData, const string aUploadedFile)
  {
    ErrorPtr err;

    string cmd;
    JsonObjectPtr o;
    if (aData->get("cmd", o)) {
      cmd = o->stringValue();
      if (cmd=="banditfileupload") {
        // create destination file name
        // - original name
        string origname = aUploadedFile;
        size_t p = aUploadedFile.rfind('/');
        if (p!=string::npos) {
          origname = aUploadedFile.substr(p+1);
        }
        string filepath = Application::sharedApplication()->dataPath(origname);
        LOG(LOG_NOTICE, "Saving uploaded file '%s' as '%s'", aUploadedFile.c_str(), filepath.c_str());
        err = copyfile(aUploadedFile, filepath);
        if (Error::isOK(err)) {
          // auto-select the file
          selectedfile = origname;
        }
      }
      else {
        err = WebError::webErr(500, "Unknown upload cmd '%s'", cmd.c_str());
      }
    }
    return err;
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
        DIR *dirP = opendir(Application::sharedApplication()->dataPath().c_str());
        struct dirent *direntP;
        if (dirP==NULL) {
          err = SysError::errNo("Cannot read data directory: ");
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
          string filepath = Application::sharedApplication()->dataPath(filename.c_str());
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
                  string newpath = Application::sharedApplication()->dataPath(newname.c_str());
                  if (rename(filepath.c_str(), newpath.c_str())!=0) {
                    err = SysError::errNo("Cannot rename file: ");
                  }
                }
              }
            }
            else if (action=="delete") {
              if (unlink(filepath.c_str())!=0) {
                err = SysError::errNo("Cannot delete file: ");
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
            else if (action=="send") {
              err = sendFile(filepath);
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
    else if (aUri=="/") {
      string uploadedfile;
      string cmd;
      if (aData->get("uploadedfile", o)) {
        uploadedfile = o->stringValue();
        actionStatus(aRequestDoneCB, processUpload(aUri, aData, uploadedfile));
        return true;
      }
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


};





int main(int argc, char **argv)
{
  // prevent debug output before application.main scans command line
  SETLOGLEVEL(LOG_EMERG);
  SETERRLEVEL(LOG_EMERG, false); // messages, if any, go to stderr
  // create app with current mainloop
  static P44BanditD application;
  // pass control
  return application.main(argc, argv);
}
