#include "StdInc.h"
#include <state/SynchronizedSceneManager.h>
#include <state/ServerGameState.h>
#include <ServerEventComponent.h>
#include <ClientRegistry.h>
#include <cmath>
#include <algorithm>

namespace fx
{
SynchronizedSceneManager::SynchronizedSceneManager()
{
}

void SynchronizedSceneManager::AttachToObject(fx::ServerInstanceBase* instance)
{
	m_instance = instance;
}

float SynchronizedSceneManager::CalculatePhaseInternal(const ServerSyncedScene& scene) const
{
	if (!scene.isRunning || scene.duration <= 0.0f)
	{
		return scene.startPhase;
	}

	auto now = std::chrono::steady_clock::now();
	float elapsedSec = std::chrono::duration<float>(now - scene.startTime).count() * scene.animRate;
	float totalProgress = scene.startPhase + (elapsedSec / scene.duration);

	if (scene.isLooping)
	{
		float p = std::fmod(totalProgress, 1.0f);
		if (p < 0.0f)
		{
			p += 1.0f;
		}
		return p;
	}
	else
	{
		if (totalProgress >= 1.0f)
		{
			return scene.holdLastFrame ? 1.0f : 0.0f;
		}
		return totalProgress;
	}
}

void SynchronizedSceneManager::SendSceneToClient(const ServerSyncedScene& scene, const fx::ClientSharedPtr& client)
{
	if (!m_instance || !client)
	{
		return;
	}

	auto sec = m_instance->GetComponent<fx::ServerEventComponent>();
	if (!sec.GetRef())
	{
		return;
	}

	SyncedSceneDataMsgPack data;
	data.sceneId = scene.sceneId;
	data.x = scene.coords.x;
	data.y = scene.coords.y;
	data.z = scene.coords.z;
	data.rx = scene.rot.x;
	data.ry = scene.rot.y;
	data.rz = scene.rot.z;
	data.rotOrder = scene.rotOrder;
	data.duration = scene.duration;
	data.animRate = scene.animRate;
	data.isLooping = scene.isLooping;
	data.holdLastFrame = scene.holdLastFrame;
	data.currentPhase = CalculatePhaseInternal(scene);
	data.serverTimestamp = msec().count();

	for (const auto& p : scene.participants)
	{
		SyncedSceneParticipantMsgPack mp;
		mp.netId = static_cast<int>(p.netId);
		mp.isPed = p.isPed;
		mp.animDict = p.animDict;
		mp.animName = p.animName;
		mp.blendInSpeed = p.blendInSpeed;
		mp.blendOutSpeed = p.blendOutSpeed;
		mp.flags = p.flags;
		mp.ragdollFlags = p.ragdollFlags;
		mp.moverBlendInDelta = p.moverBlendInDelta;
		mp.ikFlags = p.ikFlags;

		data.participants.push_back(std::move(mp));
	}

	sec->TriggerClientEvent("_cfx_internal:syncSceneCatchup", fmt::sprintf("%d", client->GetNetId()), data);
}

void SynchronizedSceneManager::SendSceneStopToClients(int sceneId)
{
	if (!m_instance)
	{
		return;
	}

	auto sec = m_instance->GetComponent<fx::ServerEventComponent>();
	if (!sec.GetRef())
	{
		return;
	}

	sec->TriggerClientEvent("_cfx_internal:syncSceneStop", std::optional<std::string_view>(), sceneId);
}

int SynchronizedSceneManager::CreateScene(const glm::vec3& coords, const glm::vec3& rot, int rotOrder, float duration, bool isLooping, bool holdLastFrame)
{
	std::unique_lock lock(m_mutex);

	int sceneId = m_nextSceneId++;
	ServerSyncedScene scene;
	scene.sceneId = sceneId;
	scene.coords = coords;
	scene.rot = rot;
	scene.rotOrder = rotOrder;
	scene.duration = duration;
	scene.animRate = 1.0f;
	scene.isLooping = isLooping;
	scene.holdLastFrame = holdLastFrame;
	scene.startTime = std::chrono::steady_clock::now();
	scene.startPhase = 0.0f;
	scene.isRunning = false;

	m_scenes[sceneId] = std::move(scene);
	return sceneId;
}

bool SynchronizedSceneManager::AddPedToScene(int sceneId, uint32_t entityHandle, const std::string& animDict, const std::string& animName, float blendInSpeed, float blendOutSpeed, int flags, int ragdollFlags, float moverBlendInDelta, int ikFlags)
{
	std::unique_lock lock(m_mutex);

	auto it = m_scenes.find(sceneId);
	if (it == m_scenes.end())
	{
		return false;
	}

	SyncedSceneParticipant p;
	p.entityHandle = entityHandle;
	p.netId = static_cast<uint16_t>(entityHandle & 0xFFFF);
	p.isPed = true;
	p.animDict = animDict;
	p.animName = animName;
	p.blendInSpeed = blendInSpeed;
	p.blendOutSpeed = blendOutSpeed;
	p.flags = flags;
	p.ragdollFlags = ragdollFlags;
	p.moverBlendInDelta = moverBlendInDelta;
	p.ikFlags = ikFlags;

	it->second.participants.push_back(std::move(p));
	m_entityToScene[entityHandle] = sceneId;
	return true;
}

bool SynchronizedSceneManager::AddEntityToScene(int sceneId, uint32_t entityHandle, const std::string& animDict, const std::string& animName, float blendInSpeed, float blendOutSpeed, int flags)
{
	std::unique_lock lock(m_mutex);

	auto it = m_scenes.find(sceneId);
	if (it == m_scenes.end())
	{
		return false;
	}

	SyncedSceneParticipant p;
	p.entityHandle = entityHandle;
	p.netId = static_cast<uint16_t>(entityHandle & 0xFFFF);
	p.isPed = false;
	p.animDict = animDict;
	p.animName = animName;
	p.blendInSpeed = blendInSpeed;
	p.blendOutSpeed = blendOutSpeed;
	p.flags = flags;

	it->second.participants.push_back(std::move(p));
	m_entityToScene[entityHandle] = sceneId;
	return true;
}

bool SynchronizedSceneManager::StartScene(int sceneId, float animRate)
{
	std::vector<fx::ClientSharedPtr> targetClients;
	ServerSyncedScene sceneCopy;

	{
		std::unique_lock lock(m_mutex);

		auto it = m_scenes.find(sceneId);
		if (it == m_scenes.end())
		{
			return false;
		}

		auto& scene = it->second;
		scene.animRate = animRate;
		scene.startTime = std::chrono::steady_clock::now();
		scene.startPhase = 0.0f;
		scene.isRunning = true;
		scene.syncedClientNetIds.clear();

		// Find clients who currently have any participant in scope
		if (m_instance)
		{
			auto sgs = m_instance->GetComponent<fx::ServerGameState>();
			auto clientRegistry = m_instance->GetComponent<fx::ClientRegistry>();

			if (sgs.GetRef() && clientRegistry.GetRef())
			{
				clientRegistry->ForAllClients([&](const fx::ClientSharedPtr& client)
				{
					if (!client)
					{
						return;
					}

					for (const auto& p : scene.participants)
					{
						if (sgs->IsClientRelevantEntity(client, p.entityHandle))
						{
							scene.syncedClientNetIds.insert(client->GetNetId());
							targetClients.push_back(client);
							break;
						}
					}
				});
			}
		}

		sceneCopy = scene;
	}

	for (const auto& client : targetClients)
	{
		SendSceneToClient(sceneCopy, client);
	}

	return true;
}

bool SynchronizedSceneManager::StopScene(int sceneId)
{
	{
		std::unique_lock lock(m_mutex);

		auto it = m_scenes.find(sceneId);
		if (it == m_scenes.end())
		{
			return false;
		}

		it->second.isRunning = false;
		it->second.syncedClientNetIds.clear();
	}

	SendSceneStopToClients(sceneId);
	return true;
}

bool SynchronizedSceneManager::SetScenePhase(int sceneId, float phase)
{
	std::unique_lock lock(m_mutex);

	auto it = m_scenes.find(sceneId);
	if (it == m_scenes.end())
	{
		return false;
	}

	it->second.startPhase = std::clamp(phase, 0.0f, 1.0f);
	it->second.startTime = std::chrono::steady_clock::now();
	return true;
}

bool SynchronizedSceneManager::SetSceneRate(int sceneId, float rate)
{
	std::unique_lock lock(m_mutex);

	auto it = m_scenes.find(sceneId);
	if (it == m_scenes.end())
	{
		return false;
	}

	// Preserve current phase before changing rate
	it->second.startPhase = CalculatePhaseInternal(it->second);
	it->second.startTime = std::chrono::steady_clock::now();
	it->second.animRate = rate;
	return true;
}

bool SynchronizedSceneManager::DoesSceneExist(int sceneId)
{
	std::shared_lock lock(m_mutex);
	return m_scenes.find(sceneId) != m_scenes.end();
}

float SynchronizedSceneManager::GetScenePhase(int sceneId)
{
	std::shared_lock lock(m_mutex);
	auto it = m_scenes.find(sceneId);
	if (it == m_scenes.end())
	{
		return 0.0f;
	}
	return CalculatePhaseInternal(it->second);
}

int SynchronizedSceneManager::GetSceneForEntity(uint32_t entityHandle)
{
	std::shared_lock lock(m_mutex);
	auto it = m_entityToScene.find(entityHandle);
	if (it != m_entityToScene.end())
	{
		return it->second;
	}
	return 0;
}

void SynchronizedSceneManager::OnEntityRelevantToClient(uint32_t entityHandle, const fx::ClientSharedPtr& client)
{
	if (!client)
	{
		return;
	}

	int clientNetId = client->GetNetId();
	ServerSyncedScene sceneCopy;
	bool shouldSend = false;

	{
		std::unique_lock lock(m_mutex);

		auto it = m_entityToScene.find(entityHandle);
		if (it == m_entityToScene.end())
		{
			return;
		}

		auto sit = m_scenes.find(it->second);
		if (sit == m_scenes.end() || !sit->second.isRunning)
		{
			return;
		}

		if (sit->second.syncedClientNetIds.insert(clientNetId).second)
		{
			// newly inserted - client has not received this scene yet!
			shouldSend = true;
			sceneCopy = sit->second;
		}
	}

	if (shouldSend)
	{
		SendSceneToClient(sceneCopy, client);
	}
}

void SynchronizedSceneManager::OnEntityOutOfScopeForClient(uint32_t entityHandle, const fx::ClientSharedPtr& client)
{
	if (!client)
	{
		return;
	}

	int clientNetId = client->GetNetId();

	std::unique_lock lock(m_mutex);
	auto it = m_entityToScene.find(entityHandle);
	if (it == m_entityToScene.end())
	{
		return;
	}

	auto sit = m_scenes.find(it->second);
	if (sit == m_scenes.end())
	{
		return;
	}

	// Check if any other participant of this scene is still relevant to the client
	if (m_instance)
	{
		auto sgs = m_instance->GetComponent<fx::ServerGameState>();
		if (sgs.GetRef())
		{
			for (const auto& p : sit->second.participants)
			{
				if (p.entityHandle != entityHandle && sgs->IsClientRelevantEntity(client, p.entityHandle))
				{
					// Another participant is still relevant, don't remove sync state yet
					return;
				}
			}
		}
	}

	// No participants left in client's scope: reset sync state so re-entering will re-sync
	sit->second.syncedClientNetIds.erase(clientNetId);
}

void SynchronizedSceneManager::OnEntityDeleted(uint32_t entityHandle)
{
	std::unique_lock lock(m_mutex);

	auto it = m_entityToScene.find(entityHandle);
	if (it == m_entityToScene.end())
	{
		return;
	}

	int sceneId = it->second;
	m_entityToScene.erase(it);

	auto sit = m_scenes.find(sceneId);
	if (sit != m_scenes.end())
	{
		auto& parts = sit->second.participants;
		parts.erase(std::remove_if(parts.begin(), parts.end(), [entityHandle](const SyncedSceneParticipant& p)
		{
			return p.entityHandle == entityHandle;
		}), parts.end());

		if (parts.empty())
		{
			sit->second.isRunning = false;
			sit->second.syncedClientNetIds.clear();
		}
	}
}

void SynchronizedSceneManager::OnClientDropped(int clientNetId)
{
	std::unique_lock lock(m_mutex);

	for (auto& pair : m_scenes)
	{
		pair.second.syncedClientNetIds.erase(clientNetId);
	}
}
}
