/*
 * SessionRMarkdown.cpp
 *
 * Copyright (C) 2009-14 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionRMarkdown.hpp"
#include "../SessionHTMLPreview.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/iostreams/filter/regex.hpp>
#include <boost/format.hpp>
#include <boost/foreach.hpp>

#include <core/FileSerializer.hpp>
#include <core/Exec.hpp>
#include <core/system/Process.hpp>

#include <r/RExec.hpp>
#include <r/RJson.hpp>
#include <r/ROptions.hpp>
#include <r/RUtil.hpp>

#include <session/SessionModuleContext.hpp>

#include "RMarkdownInstall.hpp"
#include "RMarkdownPresentation.hpp"

#define kRmdOutput "rmd_output"
#define kRmdOutputLocation "/" kRmdOutput "/"

#define kMathjaxSegment "mathjax"
#define kMathjaxBeginComment "<!-- dynamically load mathjax"

using namespace core;

namespace session {
namespace modules { 
namespace rmarkdown {

namespace {

class RenderRmd : boost::noncopyable,
                  public boost::enable_shared_from_this<RenderRmd>
{
public:
   static boost::shared_ptr<RenderRmd> create(const FilePath& targetFile,
                                              int sourceLine,
                                              const std::string& encoding)
   {
      boost::shared_ptr<RenderRmd> pRender(new RenderRmd(targetFile,
                                                         sourceLine));
      pRender->start(encoding);
      return pRender;
   }

   void terminate()
   {
      terminationRequested_ = true;
   }

   bool isRunning()
   {
      return isRunning_;
   }

   FilePath outputFile()
   {
      return outputFile_;
   }

   bool hasOutput()
   {
      return !isRunning_ && outputFile_.exists();
   }

private:
   RenderRmd(const FilePath& targetFile, int sourceLine) :
      isRunning_(false),
      terminationRequested_(false),
      targetFile_(targetFile),
      sourceLine_(sourceLine)
   {}

   void start(const std::string& encoding)
   {
      json::Object dataJson;
      getOutputFormat(targetFile_.absolutePath(), encoding, &outputFormat_);
      dataJson["output_format"] = outputFormat_;
      dataJson["target_file"] = module_context::createAliasedPath(targetFile_);
      ClientEvent event(client_events::kRmdRenderStarted, dataJson);
      module_context::enqueClientEvent(event);
      isRunning_ = true;

      performRender(encoding);
   }

   void performRender(const std::string& encoding)
   {
      // save encoding
      encoding_ = encoding;

      // R binary
      FilePath rProgramPath;
      Error error = module_context::rScriptPath(&rProgramPath);
      if (error)
      {
         terminateWithError(error);
         return;
      }

      // args
      std::vector<std::string> args;
      args.push_back("--slave");
      args.push_back("--no-save");
      args.push_back("--no-restore");
      args.push_back("-e");

      // render command
      boost::format fmt("rmarkdown::render('%1%', encoding='%2%');");
      std::string cmd = boost::str(fmt % targetFile_.filename() % encoding);
      args.push_back(cmd);

      // options
      core::system::ProcessOptions options;
      options.terminateChildren = true;
      options.workingDir = targetFile_.parent();

      // buffer the output so we can inspect it for the completed marker
      boost::shared_ptr<std::string> pAllOutput = boost::make_shared<std::string>();

      core::system::ProcessCallbacks cb;
      using namespace module_context;
      cb.onContinue = boost::bind(&RenderRmd::onRenderContinue,
                                  RenderRmd::shared_from_this());
      cb.onStdout = boost::bind(&RenderRmd::onRenderOutput,
                                RenderRmd::shared_from_this(),
                                kCompileOutputNormal,
                                _2,
                                pAllOutput);
      cb.onStderr = boost::bind(&RenderRmd::onRenderOutput,
                                RenderRmd::shared_from_this(),
                                kCompileOutputError,
                                _2,
                                pAllOutput);
      cb.onExit =  boost::bind(&RenderRmd::onRenderCompleted,
                                RenderRmd::shared_from_this(),
                               _1,
                               encoding,
                               pAllOutput);

      module_context::processSupervisor().runProgram(rProgramPath.absolutePath(),
                                                     args,
                                                     options,
                                                     cb);
   }

   bool onRenderContinue()
   {
      return !terminationRequested_;
   }

   void onRenderOutput(int type, const std::string& output,
                       boost::shared_ptr<std::string> pAllOutput)
   {
      // buffer output
      pAllOutput->append(output);
      
      enqueRenderOutput(type, output);
   }

   void onRenderCompleted(int exitStatus,
                          const std::string& encoding,
                          boost::shared_ptr<std::string> pAllOutput)
   {
      // check each line of the emitted output; if it starts with a token
      // indicating rendering is complete, store the remainder of the emitted
      // line as the file we rendered
      std::string completeMarker("Output created: ");
      std::string renderLine;
      std::stringstream outputStream(*pAllOutput);
      while (std::getline(outputStream, renderLine))
      {
         if (boost::algorithm::starts_with(renderLine, completeMarker))
         {
            std::string fileName = renderLine.substr(completeMarker.length());

            // trim any whitespace from the end of the filename (on Windows this
            // includes part of CR-LF)
            boost::algorithm::trim(fileName);

            // if the path looks absolute, use it as-is; otherwise, presume
            // it to be in the same directory as the input file
            outputFile_ = targetFile_.parent().complete(fileName);
            break;
         }
      }
      
      // consider the render to be successful if R doesn't return an error,
      // and an output file was written
      terminate(exitStatus == 0 && outputFile_.exists());
   }

   void terminateWithError(const Error& error)
   {
      std::string message =
            "Error rendering R Markdown for " +
            module_context::createAliasedPath(targetFile_) + " " +
            error.summary();
      terminateWithError(message);
   }

   void terminateWithError(const std::string& message)
   {
      enqueRenderOutput(module_context::kCompileOutputError, message);
      terminate(false);
   }

   void terminate(bool succeeded)
   {
      isRunning_ = false;
      json::Object resultJson;
      resultJson["succeeded"] = succeeded;
      resultJson["target_file"] =
            module_context::createAliasedPath(targetFile_);

      std::string outputFile = module_context::createAliasedPath(outputFile_);
      resultJson["output_file"] = outputFile;

      // A component of the output URL is the full (aliased) path of the output
      // file, on which the renderer bases requests. This path is a URL
      // component (see notes in handleRmdOutputRequest) and thus needs to
      // arrive URL-escaped.
      std::string outputUrl(kRmdOutput "/");
      std::string encodedOutputFile =
                       http::util::urlEncode(
                       http::util::urlEncode(outputFile, false), false);
#ifdef WIN32
      // One additional URL escaping pass is needed on Windows
      encodedOutputFile = http::util::urlEncode(encodedOutputFile, false);
#endif
      outputUrl.append(encodedOutputFile);
      outputUrl.append("/");
      resultJson["output_url"] = outputUrl;

      resultJson["output_format"] = outputFormat_;

      // default to no slide info
      resultJson["preview_slide"] = -1;
      resultJson["slide_navigation"] = json::Value();

      // for HTML documents, check to see whether they've been published
      if (outputFile_.extensionLowerCase() == ".html")
      {
         resultJson["rpubs_published"] =
               !module_context::previousRpubsUploadId(outputFile_).empty();
      }
      else
      {
         resultJson["rpubs_published"] = false;
      }


      // allow for format specific additions to the result json
      rmarkdown::presentation::ammendResults(
                                  outputFormat_["format_name"].get_str(),
                                  targetFile_,
                                  sourceLine_,
                                  &resultJson);

      ClientEvent event(client_events::kRmdRenderCompleted, resultJson);
      module_context::enqueClientEvent(event);
   }

   void getOutputFormat(const std::string& path,
                        const std::string& encoding,
                        json::Object* pResultJson)
   {
      // query rmarkdown for the output format
      json::Object& resultJson = *pResultJson;
      r::sexp::Protect protect;
      SEXP sexpOutputFormat;
      Error error = r::exec::RFunction("rmarkdown:::default_output_format",
                                       path, encoding)
                                      .call(&sexpOutputFormat, &protect);
      if (error)
      {
         LOG_ERROR(error);
         resultJson["format_name"] = "";
         resultJson["format_options"] = json::Value();
      }
      else
      {
         std::string formatName;
         error = r::sexp::getNamedListElement(sexpOutputFormat, "name",
                                              &formatName);
         if (error)
            LOG_ERROR(error);
         resultJson["format_name"] = formatName;

         SEXP sexpOptions;
         json::Value formatOptions;
         error = r::sexp::getNamedListSEXP(sexpOutputFormat, "options",
                                           &sexpOptions);
         if (error)
            LOG_ERROR(error);
         else
         {
            error = r::json::jsonValueFromList(sexpOptions, &formatOptions);
            if (error)
               LOG_ERROR(error);
         }

         resultJson["format_options"] = formatOptions;
      }
   }

   static void enqueRenderOutput(int type,
                                 const std::string& output)
   {
      using namespace module_context;
      CompileOutput compileOutput(type, output);
      ClientEvent event(client_events::kRmdRenderOutput,
                        compileOutputAsJson(compileOutput));
      module_context::enqueClientEvent(event);
   }

   bool isRunning_;
   bool terminationRequested_;
   FilePath targetFile_;
   int sourceLine_;
   FilePath outputFile_;
   std::string encoding_;
   json::Object outputFormat_;
};

boost::shared_ptr<RenderRmd> s_pCurrentRender_;

// replaces references to MathJax with references to our built-in resource
// handler.
// in:  script src = "http://foo/bar/Mathjax.js?abc=123"
// out: script src = "mathjax/MathJax.js?abc=123"
//
// if no MathJax use is found in the document, removes the script src statement
// entirely, so we don't incur the cost of loading MathJax in preview mode
// unless the document actually has markup.
class MathjaxFilter : public boost::iostreams::regex_filter
{
public:
   MathjaxFilter()
      // the regular expression matches any of the three tokens that look
      // like the beginning of math, and the "script src" line itself
      : boost::iostreams::regex_filter(
            boost::regex(kMathjaxBeginComment "|"
                         "\\\\\\[|\\\\\\(|<math|"
                         "^(\\s*script.src\\s*=\\s*)\"http.*?(MathJax.js[^\"]*)\""),
            boost::bind(&MathjaxFilter::substitute, this, _1)),
        hasMathjax_(false)
   {
   }

private:
   std::string substitute(const boost::cmatch& match)
   {
      std::string result;

      if (match[0] == "\\[" ||
          match[0] == "\\(" ||
          match[0] == "<math")
      {
         // if we found one of the MathJax markup start tokens, we need to emit
         // MathJax scripts
         hasMathjax_ = true;
         return match[0];
      }
      else if (match[0] == kMathjaxBeginComment)
      {
         // we found the start of the MathJax section; add the MathJax config
         // block if we're in a configuration that requires it
#ifdef __APPLE__
         return match[0];
#else
         if (session::options().programMode() != kSessionProgramModeDesktop)
            return match[0];

         result.append(kQtMathJaxConfigScript "\n");
         result.append(match[0]);
#endif
      }
      else if (hasMathjax_)
      {
         // this is the MathJax script itself; emit it if we found a start token
         result.append(match[1]);
         result.append("\"" kMathjaxSegment "/");
         result.append(match[2]);
         result.append("\"");
      }

      return result;
   }

   bool hasMathjax_;
};

bool isRenderRunning()
{
   return s_pCurrentRender_ && s_pCurrentRender_->isRunning();
}

void initPandocPath()
{
   r::exec::RFunction sysSetenv("Sys.setenv");
   sysSetenv.addParam("RSTUDIO_PANDOC", 
                      session::options().pandocPath().absolutePath());
   Error error = sysSetenv.call();
   if (error)
      LOG_ERROR(error);
}

bool haveMarkdownToHTMLOption()
{
   SEXP markdownToHTMLOption = r::options::getOption("rstudio.markdownToHTML");
   return !r::sexp::isNull(markdownToHTMLOption);
}

// when the RMarkdown package is installed, give .Rmd files the extended type
// "rmarkdown", unless there is a marker that indicates we should
// use the previous rendering strategy
std::string onDetectRmdSourceType(
      boost::shared_ptr<source_database::SourceDocument> pDoc)
{
   if (!pDoc->path().empty())
   {
      FilePath filePath = module_context::resolveAliasedPath(pDoc->path());
      if ((filePath.extensionLowerCase() == ".rmd" ||
           filePath.extensionLowerCase() == ".md") &&
          !boost::algorithm::icontains(pDoc->contents(),
                                       "<!-- rmarkdown v1 -->") &&
          !haveMarkdownToHTMLOption())
      {
         return "rmarkdown";
      }
   }
   return std::string();
}

Error getRMarkdownContext(const json::JsonRpcRequest& request,
                          json::JsonRpcResponse* pResponse)
{
   json::Object contextJson;
   contextJson["rmarkdown_installed"] = install::haveRequiredVersion();

   pResponse->setResult(contextJson);

   return Success();
}

Error renderRmd(const json::JsonRpcRequest& request,
                json::JsonRpcResponse* pResponse)
{
   int line = -1;
   std::string file, encoding;
   Error error = json::readParams(request.params, &file, &line, &encoding);
   if (error)
      return error;

   if (s_pCurrentRender_ &&
       s_pCurrentRender_->isRunning())
   {
      pResponse->setResult(false);
   }
   else
   {
      s_pCurrentRender_ = RenderRmd::create(
               module_context::resolveAliasedPath(file),
               line,
               encoding);
      pResponse->setResult(true);
   }

   return Success();
}

Error terminateRenderRmd(const json::JsonRpcRequest&,
                         json::JsonRpcResponse*)
{
   if (isRenderRunning())
      s_pCurrentRender_->terminate();

   return Success();
}

// return the path to the local copy of MathJax installed with the RMarkdown
// package
FilePath mathJaxDirectory()
{
   std::string path;
   FilePath mathJaxDir;

   // call system.file to find the appropriate path
   r::exec::RFunction findMathJax("system.file", "rmd/h/m");
   findMathJax.addParam("package", "rmarkdown");
   Error error = findMathJax.call(&path);

   // we don't expect this to fail since we shouldn't be here if RMarkdown
   // is not installed at the correct verwion
   if (error)
      LOG_ERROR(error);
   else
      mathJaxDir = FilePath(path);
   return mathJaxDir;
}

// Handles a request for RMarkdown output. This request embeds the name of
// the file to be viewed as an encoded part of the URL. For instance, requests
// to show render output for ~/abc.html and its resources look like:
//
// http://<server>/rmd_output/~%252Fabc.html/...
//
// Note that this requires two URL encoding passes at the origin, since a
// a URL decoding pass is made on the whole URL before this handler is invoked.
void handleRmdOutputRequest(const http::Request& request,
                            http::Response* pResponse)
{
   std::string path = http::util::pathAfterPrefix(request,
                                                  kRmdOutputLocation);

   // Read the desired output file name from the URL
   size_t pos = path.find('/', 1);
   if (pos == std::string::npos)
   {
      pResponse->setError(http::status::NotFound, "No output file found");
      return;
   }
   std::string outputFile = http::util::urlDecode(path.substr(0, pos));
   FilePath outputFilePath(module_context::resolveAliasedPath(outputFile));
   if (!outputFilePath.exists())
   {
      pResponse->setError(http::status::NotFound, outputFile + " not found");
      return;
   }

   // Strip the output file name from the URL
   path = path.substr(pos + 1, path.length());

   if (path.empty())
   {
      // disable caching; the request path looks identical to the browser for
      // each main request for content
      pResponse->setNoCacheHeaders();

      // serve the contents of the file with MathJax URLs mapped to our
      // own resource handler
      MathjaxFilter mathjaxFilter;
      pResponse->setFile(outputFilePath, request, mathjaxFilter);
   }
   else if (boost::algorithm::starts_with(path, kMathjaxSegment))
   {
      // serve the MathJax resource: find the requested path in the MathJax
      // directory
      pResponse->setCacheableFile(mathJaxDirectory().complete(
                                    path.substr(sizeof(kMathjaxSegment))),
                                  request);
   }
   else
   {
      // serve a file resource from the output folder
      FilePath filePath = outputFilePath.parent().childPath(path);
      pResponse->setCacheableFile(filePath, request);
   }
}

} // anonymous namespace

bool rmarkdownPackageAvailable()
{
   return r::util::hasRequiredVersion("3.0");
}

Error initialize()
{
   using boost::bind;
   using namespace module_context;

   initPandocPath();

   if (rmarkdownPackageAvailable())
   {
      module_context::events().onDetectSourceExtendedType
                                       .connect(onDetectRmdSourceType);
   }

   ExecBlock initBlock;
   initBlock.addFunctions()
      (install::initialize)
      (bind(registerRpcMethod, "get_rmarkdown_context", getRMarkdownContext))
      (bind(registerRpcMethod, "render_rmd", renderRmd))
      (bind(registerRpcMethod, "terminate_render_rmd", terminateRenderRmd))
      (bind(registerUriHandler, kRmdOutputLocation, handleRmdOutputRequest))
      (bind(module_context::sourceModuleRFile, "SessionRMarkdown.R"));

   return initBlock.execute();
}
   
} // namepsace rmarkdown
} // namespace modules
} // namesapce session

