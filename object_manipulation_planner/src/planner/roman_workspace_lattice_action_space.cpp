#include "roman_workspace_lattice_action_space.h"

// standard includes
#include <utility>

// system includes
#include <smpl/angles.h>
#include <smpl/robot_model.h>
#include <smpl/console/console.h>
#include <smpl/console/nonstd.h>
#include <smpl/graph/workspace_lattice.h>
#include <smpl/heuristic/robot_heuristic.h>

#include "smpl_assert.h"
#include "variables.h"

bool InitRomanWorkspaceLatticeActions(
    smpl::WorkspaceLattice* space,
    RomanWorkspaceLatticeActionSpace* actions)
{
    actions->space = space;

    actions->m_prims.clear();

    ///////////////////////////////////////////////////////
    // Create translational motions for the end effector //
    ///////////////////////////////////////////////////////

    auto add_xyz_prim = [&](int dx, int dy, int dz)
    {
        auto prim = smpl::MotionPrimitive{ };

        prim.type = smpl::MotionPrimitive::Type::LONG_DISTANCE;

        auto d = std::vector<double>(space->dofCount(), 0.0);
        d[VariableIndex::EE_PX] = space->resolution()[VariableIndex::EE_PX] * dx;
        d[VariableIndex::EE_PY] = space->resolution()[VariableIndex::EE_PY] * dy;
        d[VariableIndex::EE_PZ] = space->resolution()[VariableIndex::EE_PZ] * dz;
        prim.action.push_back(std::move(d));

        actions->m_prims.push_back(std::move(prim));
    };

    auto connected26 = false;
    if (connected26) {
        // create 26-connected position motions
        for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
            if (dx == 0 && dy == 0 && dz == 0) {
                continue;
            }
            add_xyz_prim(dx, dy, dz);
        }
        }
        }
    } else {
        // 2-connected motions for x, y, and z of the end effector
        add_xyz_prim(-1, 0, 0);
        add_xyz_prim(1, 0, 0);
        add_xyz_prim(0, 1, 0);
        add_xyz_prim(0, -1, 0);
        add_xyz_prim(0, 0, 1);
        add_xyz_prim(0, 0, -1);
    }

    /////////////////////////////////////////////////////////////////
    // Create 2-connected motions for all other degrees of freedom //
    /////////////////////////////////////////////////////////////////

    // create 2-connected motions for rotation and free angle motions
    //for (int a = 3; a < space->dofCount(); ++a) {
        // skip roll and pitch primitives
        // if (a == EE_QX || a == EE_QY) continue;
    // note that for torso and base motions, we attempt to move the end
    // effector by the equivalent amount

    auto enable_base_rotations = false;

    // mark degrees of freedom that should be ignored at this step...
    bool ignored[VARIABLE_COUNT] = { };
    ignored[BD_PX] = true; // handle translational base motions later
    ignored[BD_PY] = true; // handle translational base motions later
    ignored[BD_PZ] = true; // completely ignore translational z motions
    ignored[BD_QZ] = !enable_base_rotations;
    ignored[BD_QY] = true; // completely ignore rotational rp motions
    ignored[BD_QX] = true; // completely ignore rotational rp motions
    ignored[OB_P] = true; // don't move the object, what are you doing?

    for (int a = 3; a < space->dofCount(); ++a) {
        if (ignored[a]) continue;

        // Note: We want free angle motions for the base and torso to move the
        // end effector rather than keeping it at a fixed position. Here we
        // move the end effector by the closest number of discretizations in x,
        // y, z, yaw to match the change in free angle motion.

        // define a motion primitive that moves backward by one discretization
        {
            auto prim = smpl::MotionPrimitive{ };

            prim.type = smpl::MotionPrimitive::Type::LONG_DISTANCE;

            auto d = std::vector<double>(space->dofCount(), 0.0);

            d[a] = space->resolution()[a] * -1;

            if (a == TR_JP) {
                auto bins = (int)std::round(
                        space->resolution()[TR_JP] / space->resolution()[EE_QZ]);
                d[EE_QZ] = (double)-bins * space->resolution()[EE_QZ];
            }
            if (a == BD_QZ) {
                auto bins = (int)std::round(
                        space->resolution()[BD_QZ] / space->resolution()[EE_QZ]);
                d[EE_QZ] = (double)-bins * space->resolution()[EE_QZ];
            }

            prim.action.push_back(std::move(d));
            actions->m_prims.push_back(std::move(prim));
        }

        // define a motion primitive that moves forward by one discretization
        {
            auto prim = smpl::MotionPrimitive{ };

            prim.type = smpl::MotionPrimitive::Type::LONG_DISTANCE;

            auto d = std::vector<double>(space->dofCount(), 0.0);

            d[a] = space->resolution()[a] * 1;

            if (a == TR_JP) {
                auto bins = (int)std::round(
                        space->resolution()[TR_JP] / space->resolution()[EE_QZ]);
                d[EE_QZ] = (double)bins * space->resolution()[EE_QZ];
            }
            if (a == BD_QZ) {
                auto bins = (int)std::round(
                        space->resolution()[BD_QZ] / space->resolution()[EE_QZ]);
                d[EE_QZ] = (double)bins * space->resolution()[EE_QZ];
            }

            prim.action.push_back(std::move(d));
            actions->m_prims.push_back(std::move(prim));
        }
    }

    // Add motions to move the base forward/backward, left/right, and
    // diagonally. Move the end effector along with the base. Note that these
    // actions will be pruned later to enforce non-holonomic constraints.

    auto enable_base_translations = false;
    if (enable_base_translations) {
        for (int dx = -2; dx <= 2; ++dx) {
            for (int dy = -2; dy <= 2; ++dy) {
                auto prim = smpl::MotionPrimitive{ };
                prim.type = smpl::MotionPrimitive::Type::LONG_DISTANCE;

                // skip no motion
                if (dx == 0 && dy == 0) continue;

                // skip long diagonals
                if (abs(dx) == 2 & abs(dy) == 2) continue;

                // skip long translations
                if (abs(dx) == 2 & dy == 0) continue;
                if (dx == 0 & abs(dy) == 2) continue;

                auto d = std::vector<double>(space->dofCount(), 0.0);

                // apply the base motion
                d[BD_PX] = dx * space->resolution()[BD_PX];
                d[BD_PY] = dy * space->resolution()[BD_PY];

                // similar logic as above, move the end effector by the appropriate
                // number of discrete units
                auto xbins = (int)std::round(space->resolution()[BD_PX] / space->resolution()[EE_PX]);
                auto ybins = (int)std::round(space->resolution()[BD_PY] / space->resolution()[EE_PY]);
                d[EE_PX] = dx * xbins * space->resolution()[EE_PX];
                d[EE_PY] = dy * ybins * space->resolution()[EE_PY];

                prim.action.push_back(std::move(d));
                actions->m_prims.push_back(std::move(prim));
            }
        }
    }

    SMPL_INFO("%zu total motion primitives", actions->m_prims.size());
    for (auto& prim : actions->m_prims) {
        SMPL_INFO_STREAM("primitive:  " << prim.action);
    }

    return true;
}

static
bool IsSidewaysMotion(
    const smpl::WorkspaceLatticeState& state,
    const smpl::MotionPrimitive& mprim)
{
    SMPL_ASSERT(!mprim.action.empty());

    auto& last = mprim.action.back();
    SMPL_ASSERT(last.size() == VARIABLE_COUNT);

    SMPL_ASSERT(state.state.size() == VARIABLE_COUNT);

    auto dx = last[BD_PX];
    auto dy = last[BD_PY];

    if (std::fabs(dx) < 1e-6 & std::fabs(dy) < 1e-6) {
        return false;
    }

    auto thresh = smpl::to_radians(10.0);

    auto heading = atan2(dy, dx);
    auto alt_heading = heading + M_PI;
    if (smpl::shortest_angle_dist(heading, state.state[WORLD_JOINT_YAW]) > thresh &&
        smpl::shortest_angle_dist(alt_heading, state.state[WORLD_JOINT_YAW]) > thresh)
    {
        return true;
    }

    return false;
}

void RomanWorkspaceLatticeActionSpace::apply(
    const smpl::WorkspaceLatticeState& state,
    std::vector<smpl::WorkspaceAction>& actions)
{
    actions.reserve(actions.size() + m_prims.size());

    SMPL_ASSERT(state.state.size() == VARIABLE_COUNT);
    SMPL_ASSERT(state.coord.size() == VARIABLE_COUNT);

    auto cont_state = smpl::WorkspaceState{ };
    space->stateCoordToWorkspace(state.coord, cont_state);

    SMPL_DEBUG_STREAM_NAMED(G_EXPANSIONS_LOG, "  create actions for workspace state: " << cont_state);

    for (auto& prim : m_prims) {
        if (IsSidewaysMotion(state, prim)) {
            continue;
        }

        SMPL_ASSERT(!prim.action.empty());
        auto& last = prim.action.back();

#define CORRECT_EE 0

        // action moves the torso or the base theta
        if (
#if CORRECT_EE
            last[BD_PX] != 0.0 || last[BD_PY] != 0.0 ||
#endif
            last[TR_JP] != 0.0 || last[BD_QZ] != 0.0)
        {
            // apply the delta in joint space
            auto final_state = state.state;
            final_state[WORLD_JOINT_X] += last[BD_PX];
            final_state[WORLD_JOINT_Y] += last[BD_PY];
            final_state[TORSO_JOINT1] += last[TR_JP];
            final_state[WORLD_JOINT_YAW] += last[BD_QZ];

            // robot state -> workspace state
            auto tmp = smpl::WorkspaceState(space->dofCount());
            space->stateRobotToWorkspace(final_state, tmp);

            // workspace state -> discrete state -> workspace state (get cell
            // center)
            // correct for cell center
            auto get_center_state = [this](smpl::WorkspaceState& tmp)
            {
                auto ctmp = smpl::WorkspaceCoord{ };
                space->stateWorkspaceToCoord(tmp, ctmp);
                space->stateCoordToWorkspace(ctmp, tmp);
            };

            get_center_state(tmp);

            auto action = smpl::WorkspaceAction{ };
            action.push_back(std::move(tmp));
            actions.push_back(std::move(action));
        } else {
            // simply apply the delta
            auto action = smpl::WorkspaceAction{ };
            action.reserve(prim.action.size());

            auto final_state = cont_state;
            for (auto& delta_state : prim.action) {
                // increment the state
                for (size_t d = 0; d < space->dofCount(); ++d) {
                    final_state[d] += delta_state[d];
                }

                smpl::normalize_euler_zyx(&final_state[3]);

                action.push_back(final_state);
            }

            actions.push_back(std::move(action));
        }
    }

    if (m_ik_amp_enabled && space->numHeuristics() > 0) {
        // apply the adaptive motion primitive
        auto* h = space->heuristic(0);
        auto goal_dist = h->getMetricGoalDistance(
                cont_state[VariableIndex::EE_PX], cont_state[VariableIndex::EE_PY], cont_state[VariableIndex::EE_PZ]);
        if (goal_dist < m_ik_amp_thresh) {
            auto ik_sol = smpl::RobotState{ };
            if (space->m_ik_iface->computeIK(space->goal().pose, state.state, ik_sol)) {
                auto final_state = smpl::WorkspaceState{ };
                space->stateRobotToWorkspace(ik_sol, final_state);
                auto action = smpl::WorkspaceAction(1);
                action[0] = final_state;
                actions.push_back(std::move(action));
            }
        }
    }
}

