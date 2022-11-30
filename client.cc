#include <grpcpp/grpcpp.h>
#include "messages.grpc.pb.h"

#include <iostream>
#include <memory>
#include <string>

#include "opentelemetry/trace/semantic_conventions.h"
#include "opentelemetry/context/propagation/global_propagator.h"
#include "opentelemetry/context/propagation/text_map_propagator.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_exporter_factory.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/trace/simple_processor_factory.h"
#include "opentelemetry/sdk/trace/tracer_context_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/propagation/http_trace_context.h"
#include "opentelemetry/trace/provider.h"


using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::Status;

using grpc_example::Greeter;
using grpc_example::GreetRequest;
using grpc_example::GreetResponse;

class GrpcClientCarrier : public opentelemetry::context::propagation::TextMapCarrier
{
public:
  GrpcClientCarrier(ClientContext *context) : context_(context) {}
  GrpcClientCarrier() = default;
  virtual opentelemetry::nostd::string_view Get(opentelemetry::nostd::string_view) const noexcept override
  {
    return "";
  }

  virtual void Set(opentelemetry::nostd::string_view key,
                   opentelemetry::nostd::string_view value) noexcept override
  {
    std::cout << " Client ::: Adding " << key << " " << value << "\n";
    context_->AddMetadata(std::string(key), std::string(value));
  }

  ClientContext *context_;
};

class GreeterClient
{
public:
  GreeterClient(std::shared_ptr<Channel> channel) : stub_(Greeter::NewStub(channel)) {}

  std::string Greet(std::string ip, uint16_t port)
  {
    // Build gRPC Context objects and protobuf message containers
    GreetRequest request;
    GreetResponse response;
    ClientContext context;
    request.set_request("Nice to meet you!");

    opentelemetry::trace::StartSpanOptions options;
    options.kind = opentelemetry::trace::SpanKind::kClient;
    auto provider = opentelemetry::trace::Provider::GetTracerProvider();

    std::string span_name = "GreeterClient/Greet";
    auto span = provider->GetTracer("grpc")->StartSpan(
        span_name,
        {{opentelemetry::trace::SemanticConventions::kRpcSystem, "grpc"},
         {opentelemetry::trace::SemanticConventions::kRpcService, "grpc-example.GreetService"},
         {opentelemetry::trace::SemanticConventions::kRpcMethod, "Greet"},
         {opentelemetry::trace::SemanticConventions::kNetSockPeerAddr, ip},
         {opentelemetry::trace::SemanticConventions::kNetPeerPort, port}},
        options);

    auto scope = provider->GetTracer("grpc-client")->WithActiveSpan(span);

    // inject current context to grpc metadata
    auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
    GrpcClientCarrier carrier(&context);
    auto prop = opentelemetry::context::propagation::GlobalTextMapPropagator::GetGlobalPropagator();
    prop->Inject(carrier, current_ctx);

    // Send request to server
    Status status = stub_->Greet(&context, request, &response);
    if (status.ok())
    {
      span->SetStatus(opentelemetry::trace::StatusCode::kOk);
      span->SetAttribute(opentelemetry::trace::SemanticConventions::kRpcGrpcStatusCode, status.error_code());
      // Make sure to end your spans!
      span->End();
      return response.response();
    }
    else
    {
      std::cout << status.error_code() << ": " << status.error_message() << std::endl;
      span->SetStatus(opentelemetry::trace::StatusCode::kError);
      span->SetAttribute(opentelemetry::trace::SemanticConventions::kRpcGrpcStatusCode, status.error_code());
      // Make sure to end your spans!
      span->End();
      return "RPC failed";
    }
  }

private:
  std::unique_ptr<Greeter::Stub> stub_;
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
  // Default is an always-on sampler.
  std::shared_ptr<opentelemetry::sdk::trace::TracerContext> context =
      opentelemetry::sdk::trace::TracerContextFactory::Create(
          std::move(processors),
          opentelemetry::sdk::resource::Resource::Create({
              {"service.name", "otlp-apm-demo"},
              {"token", "wGcGKSZTrBHxJuPubobc"},
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

  GreeterClient greeter(grpc::CreateChannel("0.0.0.0:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  std::string response = greeter.Greet("0.0.0.0", port);
  std::cout << "grpc_server says: " << response << std::endl;

  return 0;
}
