#include <sbpl_arm_planner/bfs_heuristic.h>

#include <leatherman/viz.h>

#include <bfs3d/BFS_3D.h>

namespace sbpl {
namespace manip {

BfsHeuristic::BfsHeuristic(
    EnvironmentROBARM3D* env,
    const OccupancyGridConstPtr& grid,
    const PlanningParams* params)
:
    ManipHeuristic(env, grid, params),
    m_bfs()
{
    syncGridAndBfs();
}

BfsHeuristic::~BfsHeuristic()
{
    // empty to allow forward declaration of BFS_3D
}

bool BfsHeuristic::setGoal(int x, int y, int z)
{
    ROS_INFO("Setting the BFS heuristic goal (%d, %d, %d)", x, y, z);

    if (!m_bfs->inBounds(x, y, z)) {
        ROS_ERROR("Heuristic goal is out of BFS bounds");
        return false;
    }

    m_bfs->run(x, y, z);
    return true;
}

double BfsHeuristic::getMetricGoalDistance(double x, double y, double z)
{
    int gx, gy, gz;
    m_grid->worldToGrid(x, y, z, gx, gy, gz);
    if (!m_bfs->inBounds(gx, gy, gz)) {
        return (double)BFS_3D::WALL * m_grid->getResolution();
    }
    else {
        return (double)m_bfs->getDistance(gx, gy, gz) * m_grid->getResolution();
    }
}

int BfsHeuristic::GetGoalHeuristic(int state_id)
{
    const EnvROBARM3DHashEntry_t* state = m_manip_env->getHashEntry(state_id);
    if (state) {
        return getBfsCostToGoal(*m_bfs, state->xyz[0], state->xyz[1], state->xyz[2]);
    }
    else {
        return 0;
    }
}

int BfsHeuristic::GetStartHeuristic(int state_id)
{
    ROS_WARN_ONCE("BfsHeuristic::GetStartHeuristic unimplemented");
    return 0;
}

int BfsHeuristic::GetFromToHeuristic(int from_id, int to_id)
{
    if (m_manip_env->isGoal(to_id)) {
        return GetGoalHeuristic(from_id);
    }
    else {
        ROS_WARN_ONCE("BfsHeuristic::GetFromToHeuristic unimplemented for arbitrary state pair");
        return 0;
    }
}

visualization_msgs::MarkerArray BfsHeuristic::getWallsVisualization() const
{
    std::vector<geometry_msgs::Point> pnts;
    int dimX, dimY, dimZ;
    m_grid->getGridSize(dimX, dimY, dimZ);
    for (int z = 0; z < dimZ; z++) {
        for (int y = 0; y < dimY; y++) {
            for (int x = 0; x < dimX; x++) {
                if (m_bfs->isWall(x, y, z)) {
                    geometry_msgs::Point p;
                    m_grid->gridToWorld(x, y, z, p.x, p.y, p.z);
                    pnts.push_back(p);
                }
            }
        }
    }

    std_msgs::ColorRGBA color;
    color.r = 100.0f / 255.0f;
    color.g = 149.0f / 255.0f;
    color.b = 238.0f / 255.0f;
    color.a = 1.0f;

    visualization_msgs::Marker cubes_marker = viz::getCubesMarker(
            pnts,
            m_grid->getResolution(),
            color,
            m_grid->getReferenceFrame(),
            "bfs_walls",
            0);

    visualization_msgs::MarkerArray ma;
    ma.markers.push_back(std::move(cubes_marker));
    return ma;
}

visualization_msgs::MarkerArray BfsHeuristic::getValuesVisualization() const
{
    visualization_msgs::MarkerArray ma;
    geometry_msgs::Pose p;
    p.orientation.w = 1.0;
    int dimX, dimY, dimZ;
    m_grid->getGridSize(dimX, dimY, dimZ);
    for (int z = 0; z < dimZ; ++z) {
        for (int y = 0; y < dimY; ++y) {
            for (int x = 0; x < dimX; ++x) {
                // skip cells without valid distances from the start
                if (m_bfs->isWall(x, y, z) || m_bfs->isUndiscovered(x, y, z)) {
                    continue;
                }

                int d = m_bfs->getDistance(x, y, z);
                m_grid->gridToWorld(
                        x, y, z, p.position.x, p.position.y, p.position.z);
                double hue = d / 30.0 * 300;
                ma.markers.push_back(viz::getTextMarker(
                        p,
                        std::to_string(d),
                        0.009,
                        hue,
                        m_grid->getReferenceFrame(),
                        "bfs_values",
                        ma.markers.size()));
            }
        }
    }
    return ma;
}

void BfsHeuristic::syncGridAndBfs()
{
    int xc, yc, zc;
    m_grid->getGridSize(xc, yc, zc);
    ROS_INFO("Initializing BFS of size %d x %d x %d = %d", xc, yc, zc, xc * yc * zc);
    m_bfs.reset(new BFS_3D(xc, yc, zc));
    const int cell_count = xc * yc * zc;
    int wall_count = 0;
    for (int z = 0; z < zc; ++z) {
        for (int y = 0; y < yc; ++y) {
            for (int x = 0; x < xc; ++x) {
                if (m_grid->getDistance(x, y, z) <=
                        m_params->planning_link_sphere_radius_)
                {
                    m_bfs->setWall(x, y, z);
                    ++wall_count;
                }
            }
        }
    }

    ROS_INFO("%d/%d (%0.3f%%) walls in the bfs heuristic", wall_count, cell_count, 100.0 * (double)wall_count / cell_count);
}

int BfsHeuristic::getBfsCostToGoal(const BFS_3D& bfs, int x, int y, int z) const
{
    if (!bfs.inBounds(x, y, z)) {
        return INT_MAX;
    }
    else if (bfs.getDistance(x, y, z) == BFS_3D::WALL) {
        return INT_MAX;
    }
    else {
        return m_params->cost_per_cell_ * bfs.getDistance(x, y, z);
    }
}

} // namespace manip
} // namespace sbpl