#pragma once

#include <Client.h>
#include <ServerInstanceBase.h>
#include <state/ServerGameStatePublic.h>

#include <chrono>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <optional>
#include <msgpack.hpp>

namespace fx
{
struct SyncedSceneParticipantMsgPack
{
	int netId;
	bool isPed;
	std::string animDict;
	std::string animName;
	float blendInSpeed;
	float blendOutSpeed;
	int flags;
	int ragdollFlags;
	float moverBlendInDelta;
	int ikFlags;

	MSGPACK_DEFINE_MAP(netId, isPed, animDict, animName, blendInSpeed, blendOutSpeed, flags, ragdollFlags, moverBlendInDelta, ikFlags);
};

struct SyncedSceneDataMsgPack
{
	int sceneId;
	float x;
	float y;
	float z;
	float rx;
	float ry;
	float rz;
	int rotOrder;
	float duration;
	float animRate;
	bool isLooping;
	bool holdLastFrame;
	float currentPhase;
	uint64_t serverTimestamp;
	std::vector<SyncedSceneParticipantMsgPack> participants;

	MSGPACK_DEFINE_MAP(sceneId, x, y, z, rx, ry, rz, rotOrder, duration, animRate, isLooping, holdLastFrame, currentPhase, serverTimestamp, participants);
};

struct SyncedSceneParticipant
{
	uint32_t entityHandle = 0;
	uint16_t netId = 0;
	bool isPed = true;
	std::string animDict;
	std::string animName;
	float blendInSpeed = 8.0f;
	float blendOutSpeed = -8.0f;
	int flags = 0;
	int ragdollFlags = 0;
	float moverBlendInDelta = 1000.0f;
	int ikFlags = 0;
};

struct ServerSyncedScene
{
	int sceneId = 0;
	glm::vec3 coords{ 0.0f };
	glm::vec3 rot{ 0.0f };
	int rotOrder = 2;
	float duration = 0.0f;
	float animRate = 1.0f;
	bool isLooping = true;
	bool holdLastFrame = false;

	std::chrono::steady_clock::time_point startTime;
	float startPhase = 0.0f;
	bool isRunning = false;

	std::vector<SyncedSceneParticipant> participants;
	std::unordered_set<int> syncedClientNetIds;
};

class SynchronizedSceneManager : public fwRefCountable, public fx::IAttached<fx::ServerInstanceBase>
{
private:
	fx::ServerInstanceBase* m_instance = nullptr;
	std::shared_mutex m_mutex;
	int m_nextSceneId = 1;

	std::unordered_map<int, ServerSyncedScene> m_scenes;
	std::unordered_map<uint32_t, int> m_entityToScene;

	float CalculatePhaseInternal(const ServerSyncedScene& scene) const;
	void SendSceneToClient(const ServerSyncedScene& scene, const fx::ClientSharedPtr& client);
	void SendSceneStopToClients(int sceneId);

public:
	SynchronizedSceneManager();
	virtual ~SynchronizedSceneManager() = default;

	virtual void AttachToObject(fx::ServerInstanceBase* instance) override;

	int CreateScene(const glm::vec3& coords, const glm::vec3& rot, int rotOrder, float duration, bool isLooping, bool holdLastFrame);
	bool AddPedToScene(int sceneId, uint32_t entityHandle, const std::string& animDict, const std::string& animName, float blendInSpeed, float blendOutSpeed, int flags, int ragdollFlags, float moverBlendInDelta, int ikFlags);
	bool AddEntityToScene(int sceneId, uint32_t entityHandle, const std::string& animDict, const std::string& animName, float blendInSpeed, float blendOutSpeed, int flags);

	bool StartScene(int sceneId, float animRate = 1.0f);
	bool StopScene(int sceneId);
	bool SetScenePhase(int sceneId, float phase);
	bool SetSceneRate(int sceneId, float rate);

	bool DoesSceneExist(int sceneId);
	float GetScenePhase(int sceneId);
	int GetSceneForEntity(uint32_t entityHandle);

	void OnEntityRelevantToClient(uint32_t entityHandle, const fx::ClientSharedPtr& client);
	void OnEntityOutOfScopeForClient(uint32_t entityHandle, const fx::ClientSharedPtr& client);
	void OnEntityDeleted(uint32_t entityHandle);
	void OnClientDropped(int clientNetId);
};
}

DECLARE_INSTANCE_TYPE(fx::SynchronizedSceneManager);
