/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2024 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "creatures/npcs/npc.hpp"
#include "creatures/npcs/npcs.hpp"
#include "declarations.hpp"
#include "game/game.hpp"
#include "lua/callbacks/creaturecallback.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "map/spectators.hpp"
#include "lib/metrics/metrics.hpp"

#include "lib/thread/thread_pool.hpp"

#include <future>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <iostream>

int32_t Npc::despawnRange;
int32_t Npc::despawnRadius;

uint32_t Npc::npcAutoID = 0x80000000;

std::shared_ptr<Npc> Npc::createNpc(const std::string &name) {
	const auto &npcType = g_npcs().getNpcType(name);
	if (!npcType) {
		return nullptr;
	}
	return std::make_shared<Npc>(npcType);
}

Npc::Npc(const std::shared_ptr<NpcType> &npcType) :
	Creature(),
	strDescription(npcType->nameDescription),
	npcType(npcType) {
	defaultOutfit = npcType->info.outfit;
	currentOutfit = npcType->info.outfit;
	float multiplier = g_configManager().getFloat(RATE_NPC_HEALTH);
	health = npcType->info.health * multiplier;
	healthMax = npcType->info.healthMax * multiplier;
	baseSpeed = npcType->info.baseSpeed;
	internalLight = npcType->info.light;
	floorChange = npcType->info.floorChange;

	// register creature events
	for (const std::string &scriptName : npcType->info.scripts) {
		if (!registerCreatureEvent(scriptName)) {
			g_logger().warn("Unknown event name: {}", scriptName);
		}
	}
}

void Npc::addList() {
	g_game().addNpc(static_self_cast<Npc>());
}

void Npc::removeList() {
	g_game().removeNpc(static_self_cast<Npc>());
}

bool Npc::canInteract(const Position &pos, uint32_t range /* = 4 */) {
	if (pos.z != getPosition().z) {
		return false;
	}
	return Creature::canSee(getPosition(), pos, range, range);
}

void Npc::onCreatureAppear(std::shared_ptr<Creature> creature, bool isLogin) {
	Creature::onCreatureAppear(creature, isLogin);

	if (auto player = creature->getPlayer()) {
		onPlayerAppear(player);
	}

	// onCreatureAppear(self, creature)
	CreatureCallback callback = CreatureCallback(npcType->info.scriptInterface, getNpc());
	if (callback.startScriptInterface(npcType->info.creatureAppearEvent)) {
		callback.pushSpecificCreature(static_self_cast<Npc>());
		callback.pushCreature(creature);
	}

	if (callback.persistLuaState()) {
		return;
	}
}

void Npc::onRemoveCreature(std::shared_ptr<Creature> creature, bool isLogout) {
	Creature::onRemoveCreature(creature, isLogout);

	// onCreatureDisappear(self, creature)
	CreatureCallback callback = CreatureCallback(npcType->info.scriptInterface, getNpc());
	if (callback.startScriptInterface(npcType->info.creatureDisappearEvent)) {
		callback.pushSpecificCreature(static_self_cast<Npc>());
		callback.pushCreature(creature);
	}

	if (callback.persistLuaState()) {
		return;
	}

	if (auto player = creature->getPlayer()) {
		removeShopPlayer(player->getGUID());
		onPlayerDisappear(player);
	}

	if (spawnNpc) {
		spawnNpc->startSpawnNpcCheck();
	}
}

void Npc::onCreatureMove(const std::shared_ptr<Creature> &creature, const std::shared_ptr<Tile> &newTile, const Position &newPos, const std::shared_ptr<Tile> &oldTile, const Position &oldPos, bool teleport) {
	Creature::onCreatureMove(creature, newTile, newPos, oldTile, oldPos, teleport);

	// onCreatureMove(self, creature, oldPosition, newPosition)
	CreatureCallback callback = CreatureCallback(npcType->info.scriptInterface, getNpc());
	if (callback.startScriptInterface(npcType->info.creatureMoveEvent)) {
		callback.pushSpecificCreature(static_self_cast<Npc>());
		callback.pushCreature(creature);
		callback.pushPosition(oldPos);
		callback.pushPosition(newPos);
	}

	if (callback.persistLuaState()) {
		return;
	}

	if (creature == getNpc() && !canInteract(oldPos)) {
		resetPlayerInteractions();
		closeAllShopWindows();
	}

	if (const auto &player = creature->getPlayer()) {
		handlePlayerMove(player, newPos);
	}
}

void Npc::manageIdle() {
	if (creatureCheck && playerSpectators.empty()) {
		Game::removeCreatureCheck(static_self_cast<Npc>());
	} else if (!creatureCheck) {
		g_game().addCreatureCheck(static_self_cast<Npc>());
	}
}

void Npc::onPlayerAppear(std::shared_ptr<Player> player) {
	if (player->hasFlag(PlayerFlags_t::IgnoredByNpcs) || playerSpectators.contains(player)) {
		return;
	}
	playerSpectators.emplace(player);
	manageIdle();
}

void Npc::onPlayerDisappear(std::shared_ptr<Player> player) {
	removePlayerInteraction(player);
	if (!player->hasFlag(PlayerFlags_t::IgnoredByNpcs) && playerSpectators.contains(player)) {
		playerSpectators.erase(player);
		manageIdle();
	}
}

void Npc::onCreatureSay(std::shared_ptr<Creature> creature, SpeakClasses type, const std::string &text) {
	Creature::onCreatureSay(creature, type, text);

	if (!creature->getPlayer()) {
		return;
	}

	// onCreatureSay(self, creature, type, message)
	CreatureCallback callback = CreatureCallback(npcType->info.scriptInterface, getNpc());
	if (callback.startScriptInterface(npcType->info.creatureSayEvent)) {
		callback.pushSpecificCreature(static_self_cast<Npc>());
		callback.pushCreature(creature);
		callback.pushNumber(type);
		callback.pushString(text);
	}

	if (callback.persistLuaState()) {
		return;
	}
}

void Npc::onThinkSound(uint32_t interval) {
	if (npcType->info.soundSpeedTicks == 0) {
		return;
	}

	soundTicks += interval;
	if (soundTicks >= npcType->info.soundSpeedTicks) {
		soundTicks = 0;

		if (!npcType->info.soundVector.empty() && (npcType->info.soundChance >= static_cast<uint32_t>(uniform_random(1, 100)))) {
			auto index = uniform_random(0, npcType->info.soundVector.size() - 1);
			g_game().sendSingleSoundEffect(static_self_cast<Npc>()->getPosition(), npcType->info.soundVector[index], getNpc());
		}
	}
}

void Npc::onThink(uint32_t interval) {
	Creature::onThink(interval);

	// onThink(self, interval)
	CreatureCallback callback = CreatureCallback(npcType->info.scriptInterface, getNpc());
	if (callback.startScriptInterface(npcType->info.thinkEvent)) {
		callback.pushSpecificCreature(static_self_cast<Npc>());
		callback.pushNumber(interval);
	}

	if (callback.persistLuaState()) {
		return;
	}

	if (!npcType->canSpawn(position)) {
		g_game().removeCreature(static_self_cast<Npc>());
	}

	if (!isInSpawnRange(position)) {
		g_game().internalTeleport(static_self_cast<Npc>(), masterPos);
		resetPlayerInteractions();
		closeAllShopWindows();
	}

	if (!playerSpectators.empty()) {
		onThinkYell(interval);
		onThinkWalk(interval);
		onThinkSound(interval);
	}
}

void Npc::onPlayerBuyItem(std::shared_ptr<Player> player, uint16_t itemId, uint8_t subType, uint16_t amount, bool ignore, bool inBackpacks) {
	if (player == nullptr) {
		g_logger().error("[Npc::onPlayerBuyItem] - Player is nullptr");
		return;
	}

	// Check if the player not have empty slots or the item is not a container
	if (!ignore && (player->getFreeBackpackSlots() == 0 && (player->getInventoryItem(CONST_SLOT_BACKPACK) || (!Item::items[itemId].isContainer() || !(Item::items[itemId].slotPosition & SLOTP_BACKPACK))))) {
		player->sendCancelMessage(RETURNVALUE_NOTENOUGHROOM);
		return;
	}

	uint32_t shoppingBagPrice = 20;
	uint32_t shoppingBagSlots = 20;
	const ItemType &itemType = Item::items[itemId];
	if (std::shared_ptr<Tile> tile = ignore ? player->getTile() : nullptr; tile) {
		double slotsNedeed;
		if (itemType.stackable) {
			slotsNedeed = inBackpacks ? std::ceil(std::ceil(static_cast<double>(amount) / itemType.stackSize) / shoppingBagSlots) : std::ceil(static_cast<double>(amount) / itemType.stackSize);
		} else {
			slotsNedeed = inBackpacks ? std::ceil(static_cast<double>(amount) / shoppingBagSlots) : static_cast<double>(amount);
		}

		if ((static_cast<double>(tile->getItemList()->size()) + (slotsNedeed - player->getFreeBackpackSlots())) > 30) {
			player->sendCancelMessage(RETURNVALUE_NOTENOUGHROOM);
			return;
		}
	}

	uint32_t buyPrice = 0;
	const auto &shopVector = getShopItemVector(player->getGUID());
	for (const ShopBlock &shopBlock : shopVector) {
		if (itemType.id == shopBlock.itemId && shopBlock.itemBuyPrice != 0) {
			buyPrice = shopBlock.itemBuyPrice;
		}
	}

	uint32_t totalCost = buyPrice * amount;
	uint32_t bagsCost = 0;
	if (inBackpacks && itemType.stackable) {
		bagsCost = shoppingBagPrice * static_cast<uint32_t>(std::ceil(std::ceil(static_cast<double>(amount) / itemType.stackSize) / shoppingBagSlots));
	} else if (inBackpacks && !itemType.stackable) {
		bagsCost = shoppingBagPrice * static_cast<uint32_t>(std::ceil(static_cast<double>(amount) / shoppingBagSlots));
	}

	if (getCurrency() == ITEM_GOLD_COIN && (player->getMoney() + player->getBankBalance()) < totalCost) {
		g_logger().error("[Npc::onPlayerBuyItem (getMoney)] - Player {} have a problem for buy item {} on shop for npc {}", player->getName(), itemId, getName());
		g_logger().debug("[Information] Player {} tried to buy item {} on shop for npc {}, at position {}", player->getName(), itemId, getName(), player->getPosition().toString());
		g_metrics().addCounter("balance_decrease", totalCost, { { "player", player->getName() }, { "context", "npc_purchase" } });
		return;
	} else if (getCurrency() != ITEM_GOLD_COIN && (player->getItemTypeCount(getCurrency()) < totalCost || ((player->getMoney() + player->getBankBalance()) < bagsCost))) {
		g_logger().error("[Npc::onPlayerBuyItem (getItemTypeCount)] - Player {} have a problem for buy item {} on shop for npc {}", player->getName(), itemId, getName());
		g_logger().debug("[Information] Player {} tried to buy item {} on shop for npc {}, at position {}", player->getName(), itemId, getName(), player->getPosition().toString());
		return;
	}

	// npc:onBuyItem(player, itemId, subType, amount, ignore, inBackpacks, totalCost)
	CreatureCallback callback = CreatureCallback(npcType->info.scriptInterface, getNpc());
	if (callback.startScriptInterface(npcType->info.playerBuyEvent)) {
		callback.pushSpecificCreature(static_self_cast<Npc>());
		callback.pushCreature(player);
		callback.pushNumber(itemId);
		callback.pushNumber(subType);
		callback.pushNumber(amount);
		callback.pushBoolean(ignore);
		callback.pushBoolean(inBackpacks);
		callback.pushNumber(totalCost);
	}

	if (callback.persistLuaState()) {
		return;
	}
}

void Npc::onPlayerSellItem(std::shared_ptr<Player> player, uint16_t itemId, uint8_t subType, uint16_t amount, bool ignore) {
	uint64_t totalPrice = 0;
	onPlayerSellItem(std::move(player), itemId, subType, amount, ignore, totalPrice);
}

void Npc::onPlayerSellAllLoot(uint32_t playerId, uint16_t itemId, bool ignore, uint64_t totalPrice) {
	std::shared_ptr<Player> player = g_game().getPlayerByID(playerId);
	if (!player) {
		return;
	}
	if (itemId == ITEM_GOLD_POUCH) {
		auto container = player->getLootPouch();
		if (!container) {
			return;
		}
		bool hasMore = false;
		uint64_t toSellCount = 0;
		phmap::flat_hash_map<uint16_t, uint16_t> toSell;
		for (ContainerIterator it = container->iterator(); it.hasNext(); it.advance()) {
			if (toSellCount >= 500) {
				hasMore = true;
				break;
			}
			auto item = *it;
			if (!item) {
				continue;
			}
			toSell[item->getID()] += item->getItemAmount();
			if (item->isStackable()) {
				toSellCount++;
			} else {
				toSellCount += item->getItemAmount();
			}
		}
		for (auto &[m_itemId, amount] : toSell) {
			onPlayerSellItem(player, m_itemId, 0, amount, ignore, totalPrice, container);
		}
		auto ss = std::stringstream();
		if (totalPrice == 0) {
			ss << "You have no items in your loot pouch.";
			player->sendTextMessage(MESSAGE_TRANSACTION, ss.str());
			return;
		}
		if (hasMore) {
			g_dispatcher().scheduleEvent(
				SCHEDULER_MINTICKS, [this, playerId = player->getID(), itemId, ignore, totalPrice] { onPlayerSellAllLoot(playerId, itemId, ignore, totalPrice); }, __FUNCTION__
			);
			return;
		}
		ss << "You sold all of the items from your loot pouch for ";
		ss << totalPrice << " gold.";
		player->sendTextMessage(MESSAGE_TRANSACTION, ss.str());
		player->openPlayerContainers();
	}
}

void Npc::onPlayerSellItem(std::shared_ptr<Player> player, uint16_t itemId, uint8_t subType, uint16_t amount, bool ignore, uint64_t &totalPrice, std::shared_ptr<Cylinder> parent /*= nullptr*/) {
	if (!player) {
		return;
	}
	if (itemId == ITEM_GOLD_POUCH) {
		g_dispatcher().scheduleEvent(
			SCHEDULER_MINTICKS, [this, playerId = player->getID(), itemId, ignore] { onPlayerSellAllLoot(playerId, itemId, ignore, 0); }, __FUNCTION__
		);
		return;
	}

	uint32_t sellPrice = 0;
	const ItemType &itemType = Item::items[itemId];
	const auto &shopVector = getShopItemVector(player->getGUID());
	for (const ShopBlock &shopBlock : shopVector) {
		if (itemType.id == shopBlock.itemId && shopBlock.itemSellPrice != 0) {
			sellPrice = shopBlock.itemSellPrice;
		}
	}
	if (sellPrice == 0) {
		return;
	}

	auto toRemove = amount;
	for (const auto &item : player->getInventoryItemsFromId(itemId, ignore)) {
		if (!item || item->getTier() > 0 || item->hasImbuements()) {
			continue;
		}

		if (const auto &container = item->getContainer()) {
			if (container->size() > 0) {
				player->sendTextMessage(MESSAGE_EVENT_ADVANCE, "You must empty the container before selling it.");
				continue;
			}
		}

		if (parent && item->getParent() != parent) {
			continue;
		}

		if (!item->hasMarketAttributes()) {
			continue;
		}

		auto removeCount = std::min<uint16_t>(toRemove, item->getItemCount());

		if (g_game().internalRemoveItem(item, removeCount) != RETURNVALUE_NOERROR) {
			g_logger().error("[Npc::onPlayerSellItem] - Player {} have a problem for sell item {} on shop for npc {}", player->getName(), item->getID(), getName());
			continue;
		}

		toRemove -= removeCount;
		if (toRemove == 0) {
			break;
		}
	}

	auto totalRemoved = amount - toRemove;
	if (totalRemoved == 0) {
		return;
	}

	auto totalCost = static_cast<uint64_t>(sellPrice * totalRemoved);
	g_logger().debug("[Npc::onPlayerSellItem] - Removing items from player {} amount {} of items with id {} on shop for npc {}", player->getName(), toRemove, itemId, getName());
	if (totalRemoved > 0 && totalCost > 0) {
		if (getCurrency() == ITEM_GOLD_COIN) {
			totalPrice += totalCost;
			if (g_configManager().getBoolean(AUTOBANK)) {
				player->setBankBalance(player->getBankBalance() + totalCost);
			} else {
				g_game().addMoney(player, totalCost);
			}
			g_metrics().addCounter("balance_increase", totalCost, { { "player", player->getName() }, { "context", "npc_sale" } });
		} else {
			std::shared_ptr<Item> newItem = Item::CreateItem(getCurrency(), totalCost);
			if (newItem) {
				g_game().internalPlayerAddItem(player, newItem, true);
			}
		}
	}

	// npc:onSellItem(player, itemId, subType, amount, ignore, itemName, totalCost)
	CreatureCallback callback = CreatureCallback(npcType->info.scriptInterface, getNpc());
	if (callback.startScriptInterface(npcType->info.playerSellEvent)) {
		callback.pushSpecificCreature(static_self_cast<Npc>());
		callback.pushCreature(player);
		callback.pushNumber(itemType.id);
		callback.pushNumber(subType);
		callback.pushNumber(totalRemoved);
		callback.pushBoolean(ignore);
		callback.pushString(itemType.name);
		callback.pushNumber(totalCost);
	}

	if (callback.persistLuaState()) {
		return;
	}
}

void Npc::onPlayerCheckItem(std::shared_ptr<Player> player, uint16_t itemId, uint8_t subType) {
	if (!player) {
		return;
	}

	// onPlayerCheckItem(self, player, itemId, subType)
	CreatureCallback callback = CreatureCallback(npcType->info.scriptInterface, getNpc());
	if (callback.startScriptInterface(npcType->info.playerLookEvent)) {
		callback.pushSpecificCreature(static_self_cast<Npc>());
		callback.pushCreature(player);
		callback.pushNumber(itemId);
		callback.pushNumber(subType);
	}

	if (callback.persistLuaState()) {
		return;
	}
}

void Npc::onPlayerCloseChannel(std::shared_ptr<Creature> creature) {
	std::shared_ptr<Player> player = creature->getPlayer();
	if (!player) {
		return;
	}

	// onPlayerCloseChannel(npc, player)
	CreatureCallback callback = CreatureCallback(npcType->info.scriptInterface, getNpc());
	if (callback.startScriptInterface(npcType->info.playerCloseChannel)) {
		callback.pushSpecificCreature(static_self_cast<Npc>());
		callback.pushCreature(player);
	}

	if (callback.persistLuaState()) {
		return;
	}

	removePlayerInteraction(player);
}

void Npc::onThinkYell(uint32_t interval) {
	ThreadPool &pool = inject<ThreadPool>(); // preferably uses constructor injection or setter injection.

	// Post a task to the thread pool
	pool.detach_task([this, interval]() {
		if (npcType->info.yellSpeedTicks == 0) {
			return;
		}

		yellTicks += interval;
		if (yellTicks >= npcType->info.yellSpeedTicks) {
			yellTicks = 0;

			if (!npcType->info.voiceVector.empty() && (npcType->info.yellChance >= static_cast<uint32_t>(uniform_random(1, 33)))) {
				uint32_t index = uniform_random(0, npcType->info.voiceVector.size() - 1);
				const voiceBlock_t &vb = npcType->info.voiceVector[index];
				/*
				std::string response;
				int retryCount = 0;
				const int maxRetries = 9;

				// Retry loop for `getAiResponse`
				do {
					response = getAiResponse();
					++retryCount;
				} while (response == "Response not found." && retryCount < maxRetries);
				*/
				if (vb.yellText) {
					g_game().internalCreatureSay(static_self_cast<Npc>(), TALKTYPE_YELL, vb.text, false);
				} else {
					g_game().internalCreatureSay(static_self_cast<Npc>(), TALKTYPE_SAY, vb.text, false);
				}
			}
		}
	});
}


void Npc::onThinkWalk(uint32_t interval) {
	if (npcType->info.walkInterval == 0 || baseSpeed == 0) {
		return;
	}

	// If talking, no walking
	if (!playerInteractions.empty()) {
		walkTicks = 0;
		eventWalk = 0;
		return;
	}

	walkTicks += interval;
	if (walkTicks < npcType->info.walkInterval) {
		return;
	}

	if (Direction newDirection;
	    getRandomStep(newDirection)) {
		listWalkDir.emplace_back(newDirection);
		addEventWalk();
	}

	walkTicks = 0;
}

void Npc::onCreatureWalk() {
	Creature::onCreatureWalk();
	phmap::erase_if(playerSpectators, [this](const auto &creature) { return !this->canSee(creature->getPosition()); });
}

void Npc::onPlacedCreature() {
	loadPlayerSpectators();
}

void Npc::loadPlayerSpectators() {
	auto spec = Spectators().find<Player>(position, true);
	for (const auto &creature : spec) {
		if (!creature->getPlayer()->hasFlag(PlayerFlags_t::IgnoredByNpcs)) {
			playerSpectators.emplace(creature->getPlayer());
		}
	}
}

bool Npc::isInSpawnRange(const Position &pos) const {
	if (!spawnNpc) {
		return true;
	}

	if (Npc::despawnRadius == 0) {
		return true;
	}

	if (!SpawnsNpc::isInZone(masterPos, Npc::despawnRadius, pos)) {
		return false;
	}

	if (Npc::despawnRange == 0) {
		return true;
	}

	if (Position::getDistanceZ(pos, masterPos) > Npc::despawnRange) {
		return false;
	}

	return true;
}

void Npc::setPlayerInteraction(uint32_t playerId, uint16_t topicId /*= 0*/) {
	std::shared_ptr<Creature> creature = g_game().getCreatureByID(playerId);
	if (!creature) {
		return;
	}

	turnToCreature(creature);

	playerInteractions[playerId] = topicId;
}

void Npc::removePlayerInteraction(std::shared_ptr<Player> player) {
	if (playerInteractions.contains(player->getID())) {
		playerInteractions.erase(player->getID());
		player->closeShopWindow();
	}
}

void Npc::resetPlayerInteractions() {
	playerInteractions.clear();
}

bool Npc::canWalkTo(const Position &fromPos, Direction dir) {
	if (npcType->info.walkRadius == 0) {
		return false;
	}

	Position toPos = getNextPosition(dir, fromPos);
	if (!SpawnsNpc::isInZone(masterPos, npcType->info.walkRadius, toPos)) {
		return false;
	}

	std::shared_ptr<Tile> toTile = g_game().map.getTile(toPos);
	if (!toTile || toTile->queryAdd(0, getNpc(), 1, 0) != RETURNVALUE_NOERROR) {
		return false;
	}

	if (!floorChange && (toTile->hasFlag(TILESTATE_FLOORCHANGE) || toTile->getTeleportItem())) {
		return false;
	}

	if (!ignoreHeight && toTile->hasHeight(1)) {
		return false;
	}

	return true;
}

bool Npc::getNextStep(Direction &nextDirection, uint32_t &flags) {
	return Creature::getNextStep(nextDirection, flags);
}

bool Npc::getRandomStep(Direction &moveDirection) {
	static std::vector<Direction> directionvector {
		Direction::DIRECTION_NORTH,
		Direction::DIRECTION_WEST,
		Direction::DIRECTION_EAST,
		Direction::DIRECTION_SOUTH
	};
	std::ranges::shuffle(directionvector, getRandomGenerator());

	for (const Position &creaturePos = getPosition();
	     Direction direction : directionvector) {
		if (canWalkTo(creaturePos, direction)) {
			moveDirection = direction;
			return true;
		}
	}
	return false;
}

bool Npc::isShopPlayer(uint32_t playerGUID) const {
	return shopPlayers.find(playerGUID) != shopPlayers.end();
}

void Npc::addShopPlayer(uint32_t playerGUID, const std::vector<ShopBlock> &shopItems) {
	shopPlayers.try_emplace(playerGUID, shopItems);
}

void Npc::removeShopPlayer(uint32_t playerGUID) {
	shopPlayers.erase(playerGUID);
}

void Npc::closeAllShopWindows() {
	for (const auto &[playerGUID, shopBlock] : shopPlayers) {
		const auto &player = g_game().getPlayerByGUID(playerGUID);
		if (player) {
			player->closeShopWindow();
		}
	}
	shopPlayers.clear();
}

void Npc::handlePlayerMove(std::shared_ptr<Player> player, const Position &newPos) {
	if (!canInteract(newPos)) {
		removePlayerInteraction(player);
	}
	if (canSee(newPos)) {
		onPlayerAppear(player);
	} else {
		onPlayerDisappear(player);
	}
}
/*
// Callback function to handle the data received from the API
size_t WriteCallbacknpc(void* contents, size_t size, size_t nmemb, void* userp) {
	((std::string*)userp)->append((char*)contents, size * nmemb);
	return size * nmemb;
}

void llamaSendNpcText(std::function<void(std::string)> callback) {
	ThreadPool &pool = inject<ThreadPool>();

	// Move the callback into the lambda to avoid copying issues
	pool.detach_task([callback = std::move(callback)]() mutable {
		CURL* curl;
		CURLcode res;
		std::string readBuffer;

		// Initialize curl
		curl_global_init(CURL_GLOBAL_DEFAULT);
		curl = curl_easy_init();
		if (!curl) {
			callback("Failed to initialize CURL");
			return;
		}
		if (curl) {
			// Set the URL for the request
			curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/generate");
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L); // Request timeout in seconds
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L); // Connection timeout
			curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);

			// Prepare the JSON data to send
			std::string jsonData = R"({
                "model": "llama3.2",
"prompt": "Please, your name is NPC from the game Tibia and this is an yelling message. Please, could you talk about the weather, the beautiful environment or past glorious days. Choose one of the last themes to talk but please, write only between 10 to 15 words. Answer in a short sentence.",
                "stream": false,
                "options": {
                    "temperature": 0.9
                }
            })";

			// Set the POST data
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());

			// Specify that we are sending JSON
			struct curl_slist* headers = NULL;
			headers = curl_slist_append(headers, "Content-Type: application/json");
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

			// Set callback function to capture the response
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallbacknpc);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

			// Perform the request
			res = curl_easy_perform(curl);

			// Check if there was an error
			if (res != CURLE_OK) {
				std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
				callback("Error occurred");
			} else {
				// Extract and process the response text as you were doing previously
				std::string full_output = readBuffer;
				std::string searchTermStart = "\"response\":\"\\\"";
				std::string searchTermEnd = "\\\"";

				std::size_t startPos = full_output.find(searchTermStart);
				if (startPos != std::string::npos) {
					startPos += searchTermStart.length();
					std::size_t endPos = full_output.find(searchTermEnd, startPos);
					if (endPos != std::string::npos) {
						std::string response = full_output.substr(startPos, endPos - startPos);
						callback(response); // Call the callback with the result
					} else {
						callback("End of response not found.");
					}
				} else {
					callback("Response not found.");
				}
			}

			// Clean up
			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
		}

		curl_global_cleanup();
	});
}



// Static CURL handle and headers
static CURL* curlHandle = nullptr;
static struct curl_slist* headers = nullptr;
static std::mutex curlMutex;

// Initialize CURL once
void initializeCurl() {
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
		throw std::runtime_error("Failed to initialize cURL");
	}
	curlHandle = curl_easy_init();
	if (!curlHandle) {
		throw std::runtime_error("Failed to create cURL handle");
	}

	// Set the static headers once
	headers = curl_slist_append(headers, "Content-Type: application/json");
	curl_easy_setopt(curlHandle, CURLOPT_HTTPHEADER, headers);
}

// Cleanup function to release resources at shutdown
void cleanupCurl() {
	if (curlHandle) {
		curl_easy_cleanup(curlHandle);
		curlHandle = nullptr;
	}
	if (headers) {
		curl_slist_free_all(headers);
		headers = nullptr;
	}
	curl_global_cleanup();
}

// Write callback for cURL to capture response
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userData) {
	size_t totalSize = size * nmemb;
	userData->append((char*)contents, totalSize);
	//std::cout << "Received data: " << std::string((char*)contents, totalSize) << std::endl; // Add logging
	return totalSize;
}

void llamaSendNpcText(std::function<void(std::string)> callback) {
	// Ensure CURL is initialized
	static bool initialized = ([]() {
		initializeCurl();
		atexit(cleanupCurl); // Register cleanup to run at program exit
		return true;
	})();

	// Queue the task on the thread pool
	ThreadPool &pool = inject<ThreadPool>();
	pool.detach_task([callback = std::move(callback)]() mutable {
		std::string readBuffer;

		// Lock to ensure thread-safe access to the shared curl handle
		std::lock_guard<std::mutex> lock(curlMutex);

		// Set options specific to this request
		curl_easy_setopt(curlHandle, CURLOPT_URL, "http://localhost:11434/api/generate");
		curl_easy_setopt(curlHandle, CURLOPT_TIMEOUT, 5L); // Timeout after 5 seconds
		// curl_easy_setopt(curlHandle, CURLOPT_CONNECTTIMEOUT, 2L); // Connection timeout
		//curl_easy_setopt(curlHandle, CURLOPT_VERBOSE, 1L);
		curl_easy_setopt(curlHandle, CURLOPT_TCP_KEEPALIVE, 1L); // Enable Keep-Alive
		curl_easy_setopt(curlHandle, CURLOPT_TCP_KEEPIDLE, 1L); // Send first Keep-Alive after 5 seconds of idleness
		curl_easy_setopt(curlHandle, CURLOPT_TCP_KEEPINTVL, 1L); // Check every 5 seconds after

		std::string jsonData = R"({
            "model": "llama3.2",
            "prompt": "Please, your name is NPC from the game Tibia and this is a yelling message. Please, could you talk about the weather, the beautiful environment, or past glorious days? Choose one of the last themes to talk about but please, write only between 10 to 15 words. Answer in a short sentence.",
            "stream": false,
            "options": {
                "temperature": 0.9
            }
        })";

		curl_easy_setopt(curlHandle, CURLOPT_POSTFIELDS, jsonData.c_str());
		curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, WriteCallback);
		curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, &readBuffer);

		// Perform the request
		CURLcode res = curl_easy_perform(curlHandle);

		// Check for errors
		if (res != CURLE_OK) {
			std::cerr << "cURL request failed: " << curl_easy_strerror(res) << std::endl;
			callback("Error occurred");
		} else {
			// Process the response text
			std::string full_output = readBuffer;
			std::string searchTermStart = "\"response\":\"";
			std::string searchTermEnd = "\"";

			std::size_t startPos = full_output.find(searchTermStart);
			if (startPos != std::string::npos) {
				startPos += searchTermStart.length();
				std::size_t endPos = full_output.find(searchTermEnd, startPos);
				if (endPos != std::string::npos) {
					std::string response = full_output.substr(startPos, endPos - startPos);
					// If response has escaped quotes, remove them
					response.erase(std::remove(response.begin(), response.end(), '\\'), response.end());
					callback(response);
				} else {
					callback("End of response not found.");
				}
			} else {
				callback("Response not found.");
			}
		}
	});
}


std::string getAiResponse() {
	std::promise<std::string> promise;
	std::future<std::string> future = promise.get_future();

	llamaSendNpcText([&promise](const std::string &result) {
		promise.set_value(result);
	});

	// Wait for the result and return it
	return future.get();
}
*/
