#include <grpcpp/grpcpp.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include "messages.grpc.pb.h"

#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include "opentelemetry/trace/context.h"
#include "opentelemetry/trace/semantic_conventions.h"
#include "opentelemetry/trace/span_context_kv_iterable_view.h"
#include "opentelemetry/context/propagation/global_propagator.h"
#include "opentelemetry/context/propagation/text_map_propagator.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_exporter_factory.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/trace/simple_processor_factory.h"
#include "opentelemetry/sdk/trace/tracer_context_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/propagation/http_trace_context.h"
#include "opentelemetry/trace/provider.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;

using grpc_example::Greeter;
using grpc_example::GreetRequest;
using grpc_example::GreetResponse;

class GrpcServerCarrier : public opentelemetry::context::propagation::TextMapCarrier
{
public:
  GrpcServerCarrier(ServerContext *context) : context_(context) {}
  GrpcServerCarrier() = default;
  virtual opentelemetry::nostd::string_view Get(opentelemetry::nostd::string_view key) const noexcept override
  {
    auto it = context_->client_metadata().find({key.data(), key.size()});
    if (it != context_->client_metadata().end())
    {
      return it->second.data();
    }
    return "";
  }

  virtual void Set(opentelemetry::nostd::string_view /* key */, opentelemetry::nostd::string_view /* value */) noexcept override
  {
    // Not required for server
  }

  ServerContext *context_;
};

class GreeterServer final : public Greeter::Service
{
public:
  Status Greet(ServerContext *context, const GreetRequest *request, GreetResponse *response) override
  {
    for (auto elem : context->client_metadata())
    {
      std::cout << "ELEM: " << elem.first << " " << elem.second << "\n";
    }

    // Create a SpanOptions object and set the kind to Server to inform OpenTel.
    opentelemetry::trace::StartSpanOptions options;
    options.kind = opentelemetry::trace::SpanKind::kServer;
    auto provider = opentelemetry::trace::Provider::GetTracerProvider();

    // extract context from grpc metadata
    GrpcServerCarrier carrier(context);

    auto prop = opentelemetry::context::propagation::GlobalTextMapPropagator::GetGlobalPropagator();
    auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
    auto new_context = prop->Extract(carrier, current_ctx);
    options.parent = opentelemetry::trace::GetSpan(new_context)->GetContext();

    std::string span_name = "GreeterService/Greet";
    auto span = provider->GetTracer("grpc")->StartSpan(
        span_name,
        {{opentelemetry::trace::SemanticConventions::kRpcSystem, "grpc"},
         {opentelemetry::trace::SemanticConventions::kRpcService, "GreeterService"},
         {opentelemetry::trace::SemanticConventions::kRpcMethod, "Greet"},
         {opentelemetry::trace::SemanticConventions::kRpcGrpcStatusCode, 0}},
        options);
    auto scope = provider->GetTracer("grpc")->WithActiveSpan(span);

    // Fetch and parse whatever HTTP headers we can from the gRPC request.
    span->AddEvent("Processing client attributes");

    std::string req = request->request();
    std::cout << std::endl << "grpc_client says: " << req << std::endl;
    std::string message = "The pleasure is mine.";
    // Send response to client
    response->set_response(message);
    span->AddEvent("Response sent to client");

    span->SetStatus(opentelemetry::trace::StatusCode::kOk);
    // Make sure to end your spans!
    span->End();
    return Status::OK;
  }
}; 

int main(int argc, char **argv)
{
  //init tracer
  opentelemetry::exporter::otlp::OtlpGrpcExporterOptions opts;
  opts.endpoint = "ap-guangzhou.apm.tencentcs.com:4317";
  auto exporter = opentelemetry::exporter::otlp::OtlpGrpcExporterFactory::Create(opts);
  auto processor = opentelemetry::sdk::trace::SimpleSpanProcessorFactory::Create(std::move(exporter));
  std::vector<std::unique_ptr<opentelemetry::sdk::trace::SpanProcessor>> processors;
  processors.push_back(std::move(processor));
  std::shared_ptr<opentelemetry::sdk::trace::TracerContext> context =
      opentelemetry::sdk::trace::TracerContextFactory::Create(
          std::move(processors),
          opentelemetry::sdk::resource::Resource::Create({
              {"service.name", "otlp-apm-demo"},
              {"token", "wGcGK--------------"},
          })
      );
  std::shared_ptr<opentelemetry::trace::TracerProvider> provider = opentelemetry::sdk::trace::TracerProviderFactory::Create(context);
  // Set the global trace provider
  opentelemetry::trace::Provider::SetTracerProvider(provider);

  // set global propagator
  opentelemetry::context::propagation::GlobalTextMapPropagator::SetGlobalPropagator(
      opentelemetry::nostd::shared_ptr<opentelemetry::context::propagation::TextMapPropagator>(
          new opentelemetry::trace::propagation::HttpTraceContext()));

  uint16_t port;
  if (argc > 1)
    port = atoi(argv[1]);
  else
    port = 8800;

  std::string address("0.0.0.0:" + std::to_string(port));
  GreeterServer service;
  ServerBuilder builder;
  builder.RegisterService(&service);
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on port: " << address << std::endl;
  server->Wait();
  server->Shutdown();

  return 0;
}
