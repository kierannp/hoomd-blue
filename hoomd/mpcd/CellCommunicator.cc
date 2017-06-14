#ifdef ENABLE_MPI

#include "CellCommunicator.h"

/*!
 * \param sysdef System definition
 * \param cl MPCD cell list
 */
mpcd::CellCommunicator::CellCommunicator(std::shared_ptr<SystemDefinition> sysdef,
                                         std::shared_ptr<mpcd::CellList> cl)
    : m_sysdef(sysdef),
      m_pdata(sysdef->getParticleData()),
      m_exec_conf(m_pdata->getExecConf()),
      m_mpi_comm(m_exec_conf->getMPICommunicator()),
      m_decomposition(m_pdata->getDomainDecomposition()),
      m_cl(cl),
      m_communicating(false),
      m_needs_init(true)
    {
    m_exec_conf->msg->notice(5) << "Constructing MPCD CellCommunicator" << std::endl;
    m_cl->getSizeChangeSignal().connect<mpcd::CellCommunicator, &mpcd::CellCommunicator::slotInit>(this);
    }

mpcd::CellCommunicator::~CellCommunicator()
    {
    m_exec_conf->msg->notice(5) << "Destroying MPCD CellCommunicator" << std::endl;
    m_cl->getSizeChangeSignal().disconnect<mpcd::CellCommunicator, &mpcd::CellCommunicator::slotInit>(this);
    }

namespace mpcd
{
namespace detail
{
//! Unary operator to wrap global cell indexes into the local domain
struct LocalCellWrapOp
    {
    LocalCellWrapOp(std::shared_ptr<mpcd::CellList> cl_)
        : cl(cl_), ci(cl_->getCellIndexer()), gci(cl_->getGlobalCellIndexer())
        { }

    //! Transform the global 1D cell index into a local 1D cell index
    inline unsigned int operator()(unsigned int cell_idx)
        {
        // convert the 1D global cell index to a global cell tuple
        const uint3 cell = gci.getTriple(cell_idx);

        // convert the global cell tuple to a local cell tuple
        int3 local_cell = cl->getLocalCell(make_int3(cell.x, cell.y, cell.z));

        // wrap the local cell through the global boundaries, which should work for all reasonable cell comms.
        if (local_cell.x >= (int)gci.getW()) local_cell.x -= gci.getW();
        else if (local_cell.x < 0) local_cell.x += gci.getW();

        if (local_cell.y >= (int)gci.getH()) local_cell.y -= gci.getH();
        else if (local_cell.y < 0) local_cell.y += gci.getH();

        if (local_cell.z >= (int)gci.getD()) local_cell.z -= gci.getD();
        else if (local_cell.z < 0) local_cell.z += gci.getD();

        // convert the local cell tuple back to an index
        return ci(local_cell.x, local_cell.y, local_cell.z);
        }

    std::shared_ptr<mpcd::CellList> cl; //!< Cell list
    const Index3D ci;                   //!< Cell indexer
    const Index3D gci;                  //!< Global cell indexer
    };
} // end namespace detail
} // end namespace mpcd

void mpcd::CellCommunicator::initialize()
    {
    // obtain domain decomposition
    const Index3D& di = m_decomposition->getDomainIndexer();
    ArrayHandle<unsigned int> h_cart_ranks(m_decomposition->getCartRanks(), access_location::host, access_mode::read);
    const uint3 my_pos = m_decomposition->getGridPos();

    // use the cell list to compute the bounds
    const Index3D& ci = m_cl->getCellIndexer();
    const Index3D& global_ci = m_cl->getGlobalCellIndexer();
    auto num_comm_cells = m_cl->getNComm();
    const uint3 max_lo = make_uint3(num_comm_cells[static_cast<unsigned int>(mpcd::detail::face::west)],
                                    num_comm_cells[static_cast<unsigned int>(mpcd::detail::face::south)],
                                    num_comm_cells[static_cast<unsigned int>(mpcd::detail::face::down)]);
    const uint3 min_hi = make_uint3(ci.getW() - num_comm_cells[static_cast<unsigned int>(mpcd::detail::face::east)],
                                    ci.getH() - num_comm_cells[static_cast<unsigned int>(mpcd::detail::face::north)],
                                    ci.getD() - num_comm_cells[static_cast<unsigned int>(mpcd::detail::face::up)]);

    // loop over all cells in the grid and determine where to send them
    std::multimap<unsigned int, unsigned int> send_map;
    std::set<unsigned int> neighbors;
    for (unsigned int k=0; k < ci.getD(); ++k)
        {
        for (unsigned int j=0; j < ci.getH(); ++j)
            {
            for (unsigned int i=0; i < ci.getW(); ++i)
                {
                // skip any cells interior to the grid, which will not be communicated
                // this is wasteful loop logic, but initialize will only be called rarely
                if (i >= max_lo.x && i < min_hi.x &&
                    j >= max_lo.y && j < min_hi.y &&
                    k >= max_lo.z && k < min_hi.z)
                    continue;

                // obtain the 1D global index of this cell
                const int3 global_cell = m_cl->getGlobalCell(make_int3(i,j,k));
                const unsigned int global_cell_idx = global_ci(global_cell.x, global_cell.y, global_cell.z);

                // check which direction the cell lies off rank in x,y,z
                std::vector<int> dx = {0};
                if (i < max_lo.x)
                    dx.push_back(-1);
                else if (i >= min_hi.x)
                    dx.push_back(1);

                std::vector<int> dy = {0};
                if (j < max_lo.y)
                    dy.push_back(-1);
                else if (j >= min_hi.y)
                    dy.push_back(1);

                std::vector<int> dz = {0};
                if (k < max_lo.z)
                    dz.push_back(-1);
                else if (k >= min_hi.z)
                    dz.push_back(1);

                // generate all permutations of these neighbors for the cell
                for (auto ddx = dx.begin(); ddx != dx.end(); ++ddx)
                    {
                    for (auto ddy = dy.begin(); ddy != dy.end(); ++ddy)
                        {
                        for (auto ddz = dz.begin(); ddz != dz.end(); ++ddz)
                            {
                            // skip self
                            if (*ddx == 0 && *ddy == 0 && *ddz == 0) continue;

                            // get neighbor rank tuple
                            int3 neigh = make_int3((int)my_pos.x + *ddx,
                                                   (int)my_pos.y + *ddy,
                                                   (int)my_pos.z + *ddz);

                            // wrap neighbor through the boundaries
                            if (neigh.x < 0)
                                neigh.x += di.getW();
                            else if (neigh.x >= (int)di.getW())
                                neigh.x -= di.getW();

                            if (neigh.y < 0)
                                neigh.y += di.getH();
                            else if (neigh.y >= (int)di.getH())
                                neigh.y -= di.getH();

                            if (neigh.z < 0)
                                neigh.z += di.getD();
                            else if (neigh.z >= (int)di.getD())
                                neigh.z -= di.getD();

                            // convert neighbor to a linear rank and push it into the unique neighbor set
                            const unsigned int neigh_rank = h_cart_ranks.data[di(neigh.x,neigh.y,neigh.z)];
                            neighbors.insert(neigh_rank);
                            send_map.insert(std::make_pair(neigh_rank, global_cell_idx));
                            } // ddz
                        } // ddy
                    } // ddx
                } // i
            } // j
        } // k

    // allocate send / receive index arrays
        {
        GPUArray<unsigned int> send_idx(send_map.size(), m_exec_conf);
        m_send_idx.swap(send_idx);

        GPUArray<unsigned int> recv_idx(send_map.size(), m_exec_conf);
        m_recv_idx.swap(recv_idx);
        }

    // fill the send indexes with the global values
        {
        ArrayHandle<unsigned int> h_send_idx(m_send_idx, access_location::host, access_mode::overwrite);
        for (auto it = send_map.begin(); it != send_map.end(); ++it)
            {
            h_send_idx.data[std::distance(send_map.begin(), it)] = it->second;
            }
        }

    // flood the array of unique neighbors and count the number to send
    m_neighbors.resize(neighbors.size());
    m_begin.resize(m_neighbors.size());
    m_num_send.resize(m_neighbors.size());
    for (auto it = neighbors.begin(); it != neighbors.end(); ++it)
        {
        auto lower = send_map.lower_bound(*it);
        auto upper = send_map.upper_bound(*it);

        const unsigned int idx = std::distance(neighbors.begin(), it);
        m_neighbors[idx] = *it;
        m_begin[idx] = std::distance(send_map.begin(), lower);
        m_num_send[idx] = std::distance(lower, upper);
        }

    // send / receive the global cell indexes to be communicated with neighbors
        {
        ArrayHandle<unsigned int> h_send_idx(m_send_idx, access_location::host, access_mode::read);
        ArrayHandle<unsigned int> h_recv_idx(m_recv_idx, access_location::host, access_mode::overwrite);

        m_reqs.resize(2*m_neighbors.size());
        for (unsigned int idx=0; idx < m_neighbors.size(); ++idx)
            {
            const unsigned int offset = m_begin[idx];
            MPI_Isend(h_send_idx.data + offset, m_num_send[idx], MPI_INT, m_neighbors[idx], 0, m_mpi_comm, &m_reqs[2*idx]);
            MPI_Irecv(h_recv_idx.data + offset, m_num_send[idx], MPI_INT, m_neighbors[idx], 0, m_mpi_comm, &m_reqs[2*idx+1]);
            }
        MPI_Waitall(m_reqs.size(), m_reqs.data(), MPI_STATUSES_IGNORE);
        }

    // transform all of the global cell indexes back into local cell indexes
        {
        ArrayHandle<unsigned int> h_send_idx(m_send_idx, access_location::host, access_mode::readwrite);
        ArrayHandle<unsigned int> h_recv_idx(m_recv_idx, access_location::host, access_mode::readwrite);

        mpcd::detail::LocalCellWrapOp wrapper(m_cl);
        std::transform(h_send_idx.data, h_send_idx.data + m_send_idx.getNumElements(), h_send_idx.data, wrapper);
        std::transform(h_recv_idx.data, h_recv_idx.data + m_recv_idx.getNumElements(), h_recv_idx.data, wrapper);
        }

    // map the received cells from a rank-basis to a cell-basis
        {
        ArrayHandle<unsigned int> h_recv_idx(m_recv_idx, access_location::host, access_mode::read);

        std::multimap<unsigned int, unsigned int> cell_map;
        std::set<unsigned int> unique_cells;
        for (unsigned int idx=0; idx < m_recv_idx.getNumElements(); ++idx)
            {
            const unsigned int cell = h_recv_idx.data[idx];
            unique_cells.insert(cell);
            cell_map.insert(std::make_pair(cell, idx));
            }
        m_num_unique_cells = unique_cells.size();

        /*
         * Allocate auxiliary memory for receiving cell reordering
         */
        GPUArray<unsigned int> recv_cells(m_recv_idx.getNumElements(), m_exec_conf);
        m_recv_cells.swap(recv_cells);

        GPUArray<unsigned int> recv_cells_begin(m_num_unique_cells, m_exec_conf);
        m_recv_cells_begin.swap(recv_cells_begin);

        GPUArray<unsigned int> recv_cells_end(m_num_unique_cells, m_exec_conf);
        m_recv_cells_end.swap(recv_cells_end);

        /*
         * Write out the resorted cells from the map, and determine the range of data belonging to each received cell
         */
        ArrayHandle<unsigned int> h_recv_cells(m_recv_cells, access_location::host, access_mode::overwrite);
        ArrayHandle<unsigned int> h_recv_cells_begin(m_recv_cells_begin, access_location::host, access_mode::overwrite);
        ArrayHandle<unsigned int> h_recv_cells_end(m_recv_cells_end, access_location::host, access_mode::overwrite);
        for (auto it = cell_map.begin(); it != cell_map.end(); ++it)
            {
            h_recv_cells.data[std::distance(cell_map.begin(), it)] = it->second;
            }

        unsigned int idx=0;
        for (auto it = unique_cells.begin(); it != unique_cells.end(); ++it)
            {
            auto lower = cell_map.lower_bound(*it);
            auto upper = cell_map.upper_bound(*it);

            h_recv_cells_begin.data[idx] = std::distance(cell_map.begin(), lower);
            h_recv_cells_end.data[idx] = std::distance(cell_map.begin(), upper);

            ++idx;
            }
        }
    }

#endif // ENABLE_MPI
