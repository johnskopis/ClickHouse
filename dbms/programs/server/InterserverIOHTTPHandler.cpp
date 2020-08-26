#include "InterserverIOHTTPHandler.h"

#include <Poco/Net/HTTPBasicCredentials.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <common/logger_useful.h>
#include <Common/HTMLForm.h>
#include <Common/setThreadName.h>
#include <Compression/CompressedWriteBuffer.h>
#include <IO/ReadBufferFromIStream.h>
#include <IO/WriteBufferFromHTTPServerResponse.h>
#include <Interpreters/InterserverIOHandler.h>
#include "IServer.h"

namespace DB
{

namespace ErrorCodes
{
    extern const int ABORTED;
    extern const int TOO_MANY_SIMULTANEOUS_QUERIES;
}

std::pair<String, bool> InterserverIOHTTPHandler::checkAuthentication(Poco::Net::HTTPServerRequest & request) const
{
    auto creds = server.context().getInterserverCredential();
    const std::string DEFAULT_USER = "";
    const std::string DEFAULT_PASSWORD = "";

    String scheme, info;
    if (!request.hasCredentials())
        return creds->isValidUser(std::make_pair(DEFAULT_USER, DEFAULT_PASSWORD));

    request.getCredentials(scheme, info);

    if (scheme != "Basic")
        return {"Server requires HTTP Basic authentification but client provides another method", false};

    Poco::Net::HTTPBasicCredentials credentials(info);
    return creds->isValidUser(std::make_pair(credentials.getUsername(), credentials.getPassword()));
}

void InterserverIOHTTPHandler::processQuery(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response, Output & used_output)
{
    HTMLForm params(request);

    LOG_TRACE(log, "Request URI: " << request.getURI());

    String endpoint_name = params.get("endpoint");
    bool compress = params.get("compress") == "true";

    ReadBufferFromIStream body(request.stream());

    auto endpoint = server.context().getInterserverIOHandler().getEndpoint(endpoint_name);

    if (compress)
    {
        CompressedWriteBuffer compressed_out(*used_output.out.get());
        endpoint->processQuery(params, body, compressed_out, response);
    }
    else
    {
        endpoint->processQuery(params, body, *used_output.out.get(), response);
    }
}


void InterserverIOHTTPHandler::handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
{
    setThreadName("IntersrvHandler");

    /// In order to work keep-alive.
    if (request.getVersion() == Poco::Net::HTTPServerRequest::HTTP_1_1)
        response.setChunkedTransferEncoding(true);

    Output used_output;
    const auto & config = server.config();
    unsigned keep_alive_timeout = config.getUInt("keep_alive_timeout", 10);
    used_output.out = std::make_shared<WriteBufferFromHTTPServerResponse>(request, response, keep_alive_timeout);

    try
    {
        if (auto [message, success] = checkAuthentication(request); success)
        {
            processQuery(request, response, used_output);
            LOG_INFO(log, "Done processing query");
        }
        else
        {
            response.setStatusAndReason(Poco::Net::HTTPServerResponse::HTTP_UNAUTHORIZED);
            if (!response.sent())
                writeString(message, *used_output.out);
            LOG_WARNING(log, "Query processing failed request: '" << request.getURI() << "' authentification failed");
        }
    }
    catch (Exception & e)
    {
        if (e.code() == ErrorCodes::TOO_MANY_SIMULTANEOUS_QUERIES)
            return;

        response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);

        /// Sending to remote server was cancelled due to server shutdown or drop table.
        bool is_real_error = e.code() != ErrorCodes::ABORTED;

        std::string message = getCurrentExceptionMessage(is_real_error);
        if (!response.sent())
            writeString(message, *used_output.out);

        if (is_real_error)
            LOG_ERROR(log, message);
        else
            LOG_INFO(log, message);
    }
    catch (...)
    {
        response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
        std::string message = getCurrentExceptionMessage(false);
        if (!response.sent())
            writeString(message, *used_output.out);

        LOG_ERROR(log, message);
    }
}


}
