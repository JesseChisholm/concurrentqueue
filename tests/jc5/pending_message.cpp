#include "stdafx.h"

#include <atomic>
#include "pending_message.h"

volatile std::atomic<size_t> next_pm_id = 1;

void init_PendingMessage(PendingMessage& pm, std::size_t force_id /*= (size_t)~0*/)
{
  pm.id = (force_id == (size_t)~0) ? next_pm_id.fetch_add(1) : force_id;
  fprintf(stdout, "debug: PendingMessage.id = %u\n", pm.id);
  pm.packageName = "packageName";
  pm.messageName = "messageName";
  pm.messageBytes = { 0,1,2,3 };
  pm.customContext = std::make_shared<Context>();
  pm.customContext->sessionId = "sessionId";
  pm.customContext->host = std::make_shared<Host>();
  pm.customContext->host->id = "hostId";
  pm.customContext->program = std::make_shared<Program>();
  pm.customContext->program->id = "programId";
  pm.customContext->program->name = "programName";
  pm.customContext->program->version = "1.2.3-programVersion";
  pm.customContext->program->versionDetails = std::make_shared<VersionDetails>();
  pm.customContext->program->versionDetails->major = 1;
  pm.customContext->program->versionDetails->minor = 2;
  pm.customContext->program->versionDetails->patch = 3;
  pm.customContext->program->versionDetails->prerelease = "programVersion";
  pm.customContext->program->versionDetails->build = 0;
  pm.customContext->program->sdk = std::make_shared<Sdk>();
  pm.customContext->program->sdk->id = "sdkId";
  pm.customContext->program->sdk->name = "sdkName";
  pm.customContext->program->sdk->version = "1.2.3-sdkVersion";
  pm.customContext->program->sdk->versionDetails = std::make_shared<VersionDetails>();
  pm.customContext->program->sdk->versionDetails->major = 1;
  pm.customContext->program->sdk->versionDetails->minor = 2;
  pm.customContext->program->sdk->versionDetails->patch = 3;
  pm.customContext->program->sdk->versionDetails->prerelease = "sdkVersion";
  pm.customContext->program->sdk->versionDetails->build = 0;
}
PendingMessagePtr createPendingMessagePtr(std::size_t force_id /*= (size_t)~0*/) {
  PendingMessagePtr pm = std::make_shared<PendingMessage>();
  init_PendingMessage(*pm, force_id);
  return pm;
}
