#pragma once

#include <string>
#include <vector>

typedef struct {
  std::string id;
  // ...
} Host;

typedef struct {
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
  std::string prerelease;
  uint32_t build;
} VersionDetails;

typedef struct {
  std::string id;
  std::string name;
  std::string version;
  std::shared_ptr<VersionDetails> versionDetails;
} Sdk;

typedef struct {
  std::string id;
  std::string name;
  std::string version;
  std::shared_ptr<VersionDetails> versionDetails;
  std::shared_ptr<Sdk> sdk;
} Program;

typedef struct {
  std::string sessionId;
  std::shared_ptr<Host> host;
  std::shared_ptr<Program> program;
} Context;

typedef struct {
  std::size_t id;
  std::string packageName;
  std::string messageName;
  std::vector<unsigned char> messageBytes;
  std::shared_ptr<Context> customContext;
} PendingMessage;

typedef std::shared_ptr<PendingMessage> PendingMessagePtr;
typedef bool (*FlowControlTestFunctor)(PendingMessagePtr& pm);

void init_PendingMessage(PendingMessage& pm, std::size_t force_id = (size_t)~0);
PendingMessagePtr createPendingMessagePtr(std::size_t force_id = (size_t)~0);
