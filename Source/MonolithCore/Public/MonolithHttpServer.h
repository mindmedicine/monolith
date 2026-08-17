#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"
#include "IHttpRouter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"

class FJsonObject;
class FJsonValue;
class FMonolithToolRegistry;
struct FMonolithActionResult;

/**
 * Embedded MCP HTTP server.
 * Implements Streamable HTTP transport with JSON-RPC 2.0 dispatch.
 */
class MONOLITHCORE_API FMonolithHttpServer
{
public:
	FMonolithHttpServer();
	~FMonolithHttpServer();

	/** Start the HTTP server on the configured port */
	bool Start(int32 Port);

	/**
	 * Unbind all routes and mark the server stopped. The underlying HTTP
	 * listener is shared with the rest of the editor and is left running, so
	 * the port stays held until the process exits — a TCP probe is not a
	 * liveness check for Monolith; use monolith_status.
	 */
	void Stop();

	/** Stop then Start — useful after a silent bind failure */
	bool Restart(int32 Port);

	/** Is the server currently running? */
	bool IsRunning() const { return bIsRunning; }

	/** Get the port the server is listening on */
	int32 GetPort() const { return BoundPort; }

	/**
	 * THE tools/call projection — the single point at which an FMonolithActionResult
	 * becomes something an MCP client can see.
	 *
	 * Extracted from HandleToolsCall so it can be TESTED. Before extraction nothing in
	 * the suite touched this layer: of 49 test files, zero referenced the HTTP server,
	 * and every one of them asserted on the struct HandleToolsCall projects FROM. That
	 * made any assertion about a failure-path field vacuous as a claim about callers —
	 * `ErrorData` was asserted green in two test files while the projection deleted it
	 * on all ~42 paths that set it, for as long as the field has existed.
	 *
	 * Pure: no socket, no server state, no side effects. HandleToolsCall calls this and
	 * MonolithTransportProjectionTest calls this — deliberately the SAME function, since
	 * a test that reimplements the projection proves only that two copies agree.
	 *
	 * Returns the MCP `tools/call` result object: `{ content: [{type:"text", text}], isError }`
	 * and nothing else. The shape is fixed by the MCP spec; an error response has no
	 * structured field, which is why ErrorData and warnings ride inside `text` (see
	 * FMonolithActionResult::FormatErrorDataBlock / FormatWarningBlock).
	 *
	 * @param ActionResult  the result as returned by FMonolithToolRegistry::ExecuteAction.
	 */
	static TSharedPtr<FJsonObject> ProjectResultToToolCallPayload(const FMonolithActionResult& ActionResult);

private:
	// --- Route Handlers ---
	bool HandlePostMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleGetMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleDeleteMcp(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleHealthCheck(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	// --- JSON-RPC Processing ---
	TSharedPtr<FJsonObject> ProcessJsonRpcRequest(const TSharedPtr<FJsonObject>& Request);
	TSharedPtr<FJsonObject> HandleInitialize(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleToolsList(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleToolsCall(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandlePing(const TSharedPtr<FJsonValue>& Id);

	// --- Helpers ---
	TUniquePtr<FHttpServerResponse> MakeJsonResponse(const FString& JsonBody, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok);
	TUniquePtr<FHttpServerResponse> MakeSseResponse(const TArray<TSharedPtr<FJsonObject>>& Messages);
	// Echo Origin only when it matches the localhost allowlist. Browsers block
	// cross-origin reads when ACAO is missing, so omitting the header for
	// non-allowlisted origins is the defence — see Issue #38.
	void AddCorsHeaders(FHttpServerResponse& Response, const FHttpServerRequest& Request);

	/** Register all HTTP routes on the current HttpRouter. */
	void BindRoutes();

	/**
	 * Unbind every route we own and mark the server stopped, leaving the
	 * HttpRouter and the module's listeners untouched. Listeners are shared
	 * process-wide, so tearing them down takes Web Remote Control, PerfCounters
	 * and every other plugin's routes with us.
	 */
	void DeactivateRoutes();

	/** Probe 127.0.0.1:Port via a TCP connect to verify the listener is actually bound. */
	static bool ProbePort(int32 Port);

	// --- State ---
	TSharedPtr<IHttpRouter> HttpRouter;
	TArray<FHttpRouteHandle> RouteHandles;
	int32 BoundPort = 0;
	bool bIsRunning = false;
	FDateTime StartTime;
};
