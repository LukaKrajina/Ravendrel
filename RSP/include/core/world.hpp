#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstdint>
#include <iostream>
#include "hardware/observability.hpp"

namespace rvdlrsp::core
{

    using EntityId = uint64_t;

    struct Component
    {
        virtual ~Component() = default;
    };

    struct TransformComponent : Component
    {
        double x = 0, y = 0, z = 0;
        double qx = 0, qy = 0, qz = 0, qw = 1;
    };

    // 机器人原型实体
    struct RobotEntity
    {
        EntityId id = 0;
        std::string name;
        TransformComponent transform;
        std::unordered_map<std::string, std::shared_ptr<Component>> components;
    };

    // 统一世界描述
    class World
    {
    private:
        std::unordered_map<EntityId, std::shared_ptr<RobotEntity>> entities_;
        EntityId next_id_ = 1;

    public:
        EntityId spawn_robot(const std::string &name)
        {
            auto e = std::make_shared<RobotEntity>();
            e->id = next_id_++;
            e->name = name;
            entities_[e->id] = e;
            RVDLRSP_INFO("world") << "Spawned robot '" << name << "' (id " << e->id << ").";
            return e->id;
        }
        RobotEntity *get(EntityId id)
        {
            auto it = entities_.find(id);
            return it == entities_.end() ? nullptr : it->second.get();
        }
        size_t entity_count() const { return entities_.size(); }
    };
}