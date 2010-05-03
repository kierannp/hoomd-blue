/*
Highly Optimized Object-oriented Many-particle Dynamics -- Blue Edition
(HOOMD-blue) Open Source Software License Copyright 2008, 2009 Ames Laboratory
Iowa State University and The Regents of the University of Michigan All rights
reserved.

HOOMD-blue may contain modifications ("Contributions") provided, and to which
copyright is held, by various Contributors who have granted The Regents of the
University of Michigan the right to modify and/or distribute such Contributions.

Redistribution and use of HOOMD-blue, in source and binary forms, with or
without modification, are permitted, provided that the following conditions are
met:

* Redistributions of source code must retain the above copyright notice, this
list of conditions, and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
list of conditions, and the following disclaimer in the documentation and/or
other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of HOOMD-blue's
contributors may be used to endorse or promote products derived from this
software without specific prior written permission.

Disclaimer

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS ``AS IS''
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND/OR
ANY WARRANTIES THAT THIS SOFTWARE IS FREE OF INFRINGEMENT ARE DISCLAIMED.

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// $Id$
// $URL$
// Maintainer: joaander

/*! \file ComputeThermoGPU.cc
    \brief Contains code for the ComputeThermoGPU class
*/

#ifdef WIN32
#pragma warning( push )
#pragma warning( disable : 4103 4244 )
#endif

#include "ComputeThermoGPU.h"
#include "ComputeThermoGPU.cuh"

#include <boost/python.hpp>
using namespace boost::python;
#include <boost/bind.hpp>
using namespace boost;

#include <iostream>
using namespace std;

/*! \param sysdef System for which to compute thermodynamic properties
    \param group Subset of the system over which properties are calculated
    \param suffix Suffix to append to all logged quantity names
*/
ComputeThermoGPU::ComputeThermoGPU(boost::shared_ptr<SystemDefinition> sysdef,
                                   boost::shared_ptr<ParticleGroup> group,
                                   const std::string& suffix)
    : ComputeThermo(sysdef, group, suffix)
    {
    m_block_size = 512;
    m_num_blocks = m_group->getNumMembers() / m_block_size + 1;
    
    GPUArray< float4 > scratch(m_num_blocks, exec_conf);
    m_scratch.swap(scratch);
    }



/*! Computes all thermodynamic properties of the system in one fell swoop, on the GPU.
*/
void ComputeThermoGPU::computeProperties()
    {
    unsigned int group_size = m_group->getNumMembers();
    // just drop out if the group is an empty group
    if (group_size == 0)
        return;
    
    if (m_prof) m_prof->push("Thermo");
    
    assert(m_pdata);
    assert(m_ndof != 0);
    
    // access the particle data
    vector<gpu_pdata_arrays>& d_pdata = m_pdata->acquireReadOnlyGPU();
    gpu_boxsize box = m_pdata->getBoxGPU();
    
    // access the net force, pe, and virial
    const GPUArray< Scalar4 >& net_force = m_pdata->getNetForce();
    const GPUArray< Scalar >& net_virial = m_pdata->getNetVirial();
    ArrayHandle<Scalar4> d_net_force(net_force, access_location::device, access_mode::read);
    ArrayHandle<Scalar> d_net_virial(net_virial, access_location::device, access_mode::read);
    ArrayHandle<float4> d_scratch(m_scratch, access_location::device, access_mode::overwrite);
    ArrayHandle<float> d_properties(m_properties, access_location::device, access_mode::overwrite);
    
    // access the group
    ArrayHandle< unsigned int > d_index_array(m_group->getIndexArray(), access_location::device, access_mode::read);
    
    // build up args list
    compute_thermo_args args;
    args.d_net_force = d_net_force.data;
    args.d_net_virial = d_net_virial.data;
    args.ndof = m_ndof;
    args.D = m_sysdef->getNDimensions();
    args.d_scratch = d_scratch.data;
    args.block_size = m_block_size;
    args.n_blocks = m_num_blocks;
    
    // perform the computation on the GPU
    exec_conf.gpu[0]->call(bind(gpu_compute_thermo, d_properties.data,
                                                    d_pdata[0],
                                                    d_index_array.data,
                                                    group_size,
                                                    box,
                                                    args));
    
    m_pdata->release();

    if (m_prof) m_prof->pop();
    }

void export_ComputeThermoGPU()
    {
    class_<ComputeThermoGPU, boost::shared_ptr<ComputeThermoGPU>, bases<ComputeThermo>, boost::noncopyable >
    ("ComputeThermoGPU", init< boost::shared_ptr<SystemDefinition>,
                         boost::shared_ptr<ParticleGroup>,
                         const std::string& >())
    ;
    }

#ifdef WIN32
#pragma warning( pop )
#endif
