#include "stdafx.h"

#include <atomic>
#include "pending_message.h"

volatile std::atomic<size_t> next_pm_id = 1;

void init_Context(std::shared_ptr<Context>& ctx)
{
  ctx->sessionId = "sessionId";
  ctx->host = std::make_shared<Host>();
  ctx->host->id = "hostId";
  ctx->program = std::make_shared<Program>();
  ctx->program->id = "programId";
  ctx->program->name = "programName";
  ctx->program->version = "1.2.3-programVersion";
  ctx->program->versionDetails = std::make_shared<VersionDetails>();
  ctx->program->versionDetails->major = 1;
  ctx->program->versionDetails->minor = 2;
  ctx->program->versionDetails->patch = 3;
  ctx->program->versionDetails->prerelease = "programVersion";
  ctx->program->versionDetails->build = 0;
  ctx->program->sdk = std::make_shared<Sdk>();
  ctx->program->sdk->id = "sdkId";
  ctx->program->sdk->name = "sdkName";
  ctx->program->sdk->version = "1.2.3-sdkVersion";
  ctx->program->sdk->versionDetails = std::make_shared<VersionDetails>();
  ctx->program->sdk->versionDetails->major = 1;
  ctx->program->sdk->versionDetails->minor = 2;
  ctx->program->sdk->versionDetails->patch = 3;
  ctx->program->sdk->versionDetails->prerelease = "sdkVersion";
  ctx->program->sdk->versionDetails->build = 0;
}
void init_PendingMessage(PendingMessage& pm, std::size_t force_id /*= (size_t)~0*/)
{
  pm.id = (force_id == (size_t)~0) ? next_pm_id.fetch_add(1) : force_id;
  //fprintf(stdout, "debug: PendingMessage.id = %u\n", pm.id);
  pm.packageName = "packageName";
  pm.messageName = "messageName";
  pm.envelope = std::make_shared<Envelope>();
  pm.envelope->packageName = "packageName";
  pm.envelope->messageName = "messageName";
  pm.envelope->messageBytes = { 0,1,2,3 };
  pm.envelope->context = std::make_shared<Context>();
  init_Context(pm.envelope->context);
  pm.customContext = std::make_shared<Context>();
  init_Context(pm.customContext);
}
PendingMessagePtr createPendingMessagePtr(std::size_t force_id /*= (size_t)~0*/) {
  PendingMessagePtr pm = std::make_shared<PendingMessage>();
  init_PendingMessage(*pm, force_id);
  return pm;
}
