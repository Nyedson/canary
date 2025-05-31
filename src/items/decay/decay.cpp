/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2024 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "items/decay/decay.hpp"

#include "creatures/players/player.hpp"
#include "game/game.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "lib/di/container.hpp"

Decay &Decay::getInstance() {
	return inject<Decay>();
}

void Decay::startDecay(const std::shared_ptr<Item>& item) {
	if (!item) {
		return;
	}

	const auto decayState = item->getDecaying();
	if (decayState == DECAYING_STOPPING || (!item->canDecay() && decayState == DECAYING_TRUE)) {
		stopDecay(item);
		return;
	}

	if (!item->canDecay() || decayState == DECAYING_TRUE) {
		return;
	}

	const int64_t duration = item->getAttribute<int64_t>(ItemAttribute_t::DURATION);
	if (duration <= 0 && item->hasAttribute(ItemAttribute_t::DURATION)) {
		internalDecayItem(item);
		return;
	}

	const int64_t timestamp = OTSYS_TIME() + duration;

	// Só interrompe decay anterior se timestamp for diferente
	if (item->hasAttribute(ItemAttribute_t::DURATION_TIMESTAMP)) {
		const int64_t currentTimestamp = item->getAttribute<int64_t>(ItemAttribute_t::DURATION_TIMESTAMP);
		if (currentTimestamp != timestamp) {
			stopDecay(item);
		}
	}

	item->setDecaying(DECAYING_TRUE);
	item->setAttribute(ItemAttribute_t::DURATION_TIMESTAMP, timestamp);
	decayMap[timestamp].push_back(item);

	if (decayMap.size() == 1 || timestamp < nextDecayTimestamp) {
		// Novo menor timestamp, reagendar evento
		g_dispatcher().stopEvent(eventId);
		eventId = g_dispatcher().scheduleEvent(
			std::max<int32_t>(SCHEDULER_MINTICKS, duration),
			[this] { checkDecay(); },
			"Decay::checkDecay"
		);
		nextDecayTimestamp = timestamp;
	}
}

void Decay::stopDecay(const std::shared_ptr<Item>& item) {
	if (!item) {
		return;
	}

	if (!item->hasAttribute(ItemAttribute_t::DECAYSTATE)) {
		return;
	}

	const int64_t timestamp = item->getAttribute<int64_t>(ItemAttribute_t::DURATION_TIMESTAMP);

	auto it = decayMap.find(timestamp);
	if (it != decayMap.end()) {
		auto& decayItems = it->second;
		auto itItem = std::find(decayItems.begin(), decayItems.end(), item);
		if (itItem != decayItems.end()) {
			decayItems.erase(itItem);
			if (decayItems.empty()) {
				decayMap.erase(it);
				if (decayMap.empty()) {
					nextDecayTimestamp = 0;
				}
			}
		}
	}

	item->removeAttribute(ItemAttribute_t::DECAYSTATE);
	item->removeAttribute(ItemAttribute_t::DURATION_TIMESTAMP);
}

void Decay::checkDecay() {
	const int64_t now = OTSYS_TIME();

	tempItems.clear();

	auto it = decayMap.begin();
	while (it != decayMap.end() && it->first <= now) {
		auto& decayItems = it->second;
		tempItems.insert(tempItems.end(), decayItems.begin(), decayItems.end());
		it = decayMap.erase(it);
	}

	for (const auto& item : tempItems) {
		item->setDecaying(DECAYING_FALSE);

		if (!item->canDecay()) {
			item->setDuration(item->getDuration()); // reinicia contagem se necessário
		} else {
			internalDecayItem(item);
		}
	}

	// Agendar o próximo decay, se houver
	if (it != decayMap.end()) {
		const int64_t delay = std::max<int32_t>(SCHEDULER_MINTICKS, static_cast<int32_t>(it->first - now));
		eventId = g_dispatcher().scheduleEvent(delay, [this] { checkDecay(); }, "Decay::checkDecay");
		nextDecayTimestamp = it->first;
	} else {
		nextDecayTimestamp = 0; // Nada mais a processar
	}
}

void Decay::internalDecayItem(const std::shared_ptr<Item> &item) {
	if (!item) {
		return;
	}

	const ItemType &it = Item::items[item->getID()];
	// Remove the item and halt the decay process if a player triggers a bug where the item's decay ID matches its equip or de-equip transformation ID
	if (it.id == it.transformEquipTo || it.id == it.transformDeEquipTo) {
		g_game().internalRemoveItem(item);
		const auto &player = item->getHoldingPlayer();
		if (player) {
			g_logger().error("[{}] - internalDecayItem failed to player {}, item id is same from transform equip/deequip, "
			                 " item id: {}, equip to id: '{}', deequip to id '{}'",
			                 __FUNCTION__, player->getName(), it.id, it.transformEquipTo, it.transformDeEquipTo);
		}
		return;
	}

	if (it.decayTo != 0) {
		const auto &player = item->getHoldingPlayer();
		if (player) {
			bool needUpdateSkills = false;
			for (int32_t i = SKILL_FIRST; i <= SKILL_LAST; ++i) {
				if (it.abilities && item->getSkill(static_cast<skills_t>(i)) != 0) {
					needUpdateSkills = true;
					player->setVarSkill(static_cast<skills_t>(i), -item->getSkill(static_cast<skills_t>(i)));
				}
			}

			if (needUpdateSkills) {
				player->sendSkills();
			}

			bool needUpdateStats = false;
			for (int32_t s = STAT_FIRST; s <= STAT_LAST; ++s) {
				if (item->getStat(static_cast<stats_t>(s)) != 0) {
					needUpdateStats = true;
					needUpdateSkills = true;
					player->setVarStats(static_cast<stats_t>(s), -item->getStat(static_cast<stats_t>(s)));
				}
				if (it.abilities && it.abilities->statsPercent[s] != 0) {
					needUpdateStats = true;
					player->setVarStats(static_cast<stats_t>(s), -static_cast<int32_t>(player->getDefaultStats(static_cast<stats_t>(s)) * ((it.abilities->statsPercent[s] - 100) / 100.f)));
				}
			}

			if (needUpdateStats) {
				player->sendStats();
			}

			if (needUpdateSkills) {
				player->sendSkills();
			}
		}
		g_game().transformItem(item, static_cast<uint16_t>(it.decayTo));
	} else {
		if (item->isLoadedFromMap()) {
			return;
		}

		ReturnValue ret = g_game().internalRemoveItem(item);
		if (ret != RETURNVALUE_NOERROR) {
			g_logger().error("[Decay::internalDecayItem] - internalDecayItem failed, "
			                 "error code: {}, item id: {}",
			                 static_cast<uint32_t>(ret), item->getID());
		}
	}
}
