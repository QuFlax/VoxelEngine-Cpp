#include "BlocksController.hpp"

#include "../voxels/voxel.hpp"
#include "../voxels/Block.hpp"
#include "../voxels/Chunk.hpp"
#include "../voxels/Chunks.hpp"
#include "../world/Level.hpp"
#include "../world/World.hpp"
#include "../content/Content.hpp"
#include "../lighting/Lighting.hpp"
#include "../util/timeutil.hpp"
#include "../maths/fastmaths.hpp"
#include "../items/Inventory.hpp"
#include "../items/Inventories.hpp"

#include "scripting/scripting.hpp"

Clock::Clock(int tickRate, int tickParts)
    : tickRate(tickRate),
      tickParts(tickParts) {
}

bool Clock::update(float delta) {
    tickTimer += delta;
    float delay = 1.0f / float(tickRate);    
    if (tickTimer > delay || tickPartsUndone) {
        if (tickPartsUndone) {
            tickPartsUndone--;
        } else {
            tickTimer = fmod(tickTimer, delay);
            tickPartsUndone = tickParts-1;
        }
        return true;
    }
    return false;
}

int Clock::getParts() const {
    return tickParts;
}

int Clock::getPart() const {
    return tickParts-tickPartsUndone-1;
}

int Clock::getTickRate() const {
    return tickRate;
}

int Clock::getTickId() const {
    return tickId;
}

BlocksController::BlocksController(Level* level, uint padding) 
    : level(level), 
      chunks(level->chunks.get()), 
      lighting(level->lighting.get()),
      randTickClock(20, 3),
      blocksTickClock(20, 1),
      worldTickClock(20, 1),
      padding(padding) {
}

void BlocksController::updateSides(int x, int y, int z) {
    updateBlock(x-1, y, z);
    updateBlock(x+1, y, z);
    updateBlock(x, y-1, z);
    updateBlock(x, y+1, z);
    updateBlock(x, y, z-1);
    updateBlock(x, y, z+1);
}

void BlocksController::breakBlock(Player* player, const Block* def, int x, int y, int z) {
    chunks->set(x,y,z, 0, {});
    lighting->onBlockSet(x,y,z, 0);
    if (def->rt.funcsset.onbroken) {
        scripting::on_block_broken(player, def, x, y, z);
    }
    updateSides(x, y, z);
}

void BlocksController::updateBlock(int x, int y, int z) {
    voxel* vox = chunks->get(x, y, z);
    if (vox == nullptr)
        return;
    auto def = level->content->getIndices()->blocks.get(vox->id);
    if (def->grounded && !chunks->isSolidBlock(x, y-1, z)) {
        breakBlock(nullptr, def, x, y, z);
        return;
    }
    if (def->rt.funcsset.update) {
        scripting::update_block(def, x, y, z);
    }
}

void BlocksController::update(float delta) {
    if (randTickClock.update(delta)) {
        randomTick(randTickClock.getPart(), randTickClock.getParts());
    }
    if (blocksTickClock.update(delta)) {
        onBlocksTick(blocksTickClock.getPart(), blocksTickClock.getParts());
    }
    if (worldTickClock.update(delta)) {
        scripting::on_world_tick();
    }
}

void BlocksController::onBlocksTick(int tickid, int parts) {
    auto content = level->content;
    auto indices = content->getIndices();
    int tickRate = blocksTickClock.getTickRate();
    for (size_t id = 0; id < indices->blocks.count(); id++) {
        if ((id + tickid) % parts != 0)
            continue;
        auto def = indices->blocks.get(id);
        auto interval = def->tickInterval;
        if (def->rt.funcsset.onblockstick && tickid / parts % interval == 0) {
            scripting::on_blocks_tick(def, tickRate / interval);
        }
    }
}

void BlocksController::randomTick(int tickid, int parts) {
    const int w = chunks->w;
    const int d = chunks->d;
    int segments = 4;
    int segheight = CHUNK_H / segments;
    auto indices = level->content->getIndices();
    
    for (uint z = padding; z < d-padding; z++){
        for (uint x = padding; x < w-padding; x++){
            int index = z * w + x;
            if ((index + tickid) % parts != 0) {
                continue;
            }
            auto& chunk = chunks->chunks[index];
            if (chunk == nullptr || !chunk->flags.lighted) {
                continue;
            }
            for (int s = 0; s < segments; s++) {
                for (int i = 0; i < 4; i++) {
                    int bx = random.rand() % CHUNK_W;
                    int by = random.rand() % segheight + s * segheight;
                    int bz = random.rand() % CHUNK_D;
                    const voxel& vox = chunk->voxels[(by * CHUNK_D + bz) * CHUNK_W + bx];
                    Block* block = indices->blocks.get(vox.id);
                    if (block->rt.funcsset.randupdate) {
                        scripting::random_update_block(
                            block, 
                            chunk->x * CHUNK_W + bx, by, 
                            chunk->z * CHUNK_D + bz
                        );
                    }
                }
            }
        }
    }
}

int64_t BlocksController::createBlockInventory(int x, int y, int z) {
    auto chunk = chunks->getChunkByVoxel(x, y, z);
    if (chunk == nullptr) {
        return 0;
    }
    int lx = x - chunk->x * CHUNK_W;
    int lz = z - chunk->z * CHUNK_D;
    auto inv = chunk->getBlockInventory(lx, y, lz);
    if (inv == nullptr) {
        auto indices = level->content->getIndices();
        auto def = indices->blocks.get(chunk->voxels[vox_index(lx, y, lz)].id);
        int invsize = def->inventorySize;
        if (invsize == 0) {
            return 0;
        }
        inv = level->inventories->create(invsize);
        chunk->addBlockInventory(inv, lx, y, lz);
    }
    return inv->getId();
}

void BlocksController::bindInventory(int64_t invid, int x, int y, int z) {
    auto chunk = chunks->getChunkByVoxel(x, y, z);
    if (chunk == nullptr) {
        throw std::runtime_error("block does not exists");
    }
    if (invid <= 0) {
        throw std::runtime_error("unable to bind virtual inventory");
    }
    int lx = x - chunk->x * CHUNK_W;
    int lz = z - chunk->z * CHUNK_D;
    chunk->addBlockInventory(level->inventories->get(invid), lx, y, lz);
}

void BlocksController::unbindInventory(int x, int y, int z) {
    auto chunk = chunks->getChunkByVoxel(x, y, z);
    if (chunk == nullptr) {
        throw std::runtime_error("block does not exists");
    }
    int lx = x - chunk->x * CHUNK_W;
    int lz = z - chunk->z * CHUNK_D;
    chunk->removeBlockInventory(lx, y, lz);
}
