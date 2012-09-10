/** 
 *  @file kad.cpp
 *
 *  This file manages the KAD lookup algorithm.   
 *
 *  This algorithm depends upon a node being able to return a list of N 
 *  closest active node IDs.  
 *
 *
 */
#include <tornet/node.hpp>
#include <tornet/kad.hpp>

namespace tn { 

  kad_search::kad_search( const node::ptr& local_node, const fc::sha1& target, uint32_t n, uint32_t p ) 
  :m_n(n),m_p(p),m_node(local_node),m_target(target),m_target_dist( local_node->get_id()^target )
  {
     m_cur_status = idle;
  }

  void kad_search::start() {
     m_current_results.clear();
     m_cur_status   = searching;
     slog( "searching for %d nodes near %s", m_n, fc::string(m_target).c_str() );
     auto nn = m_node->find_nodes_near( m_target, m_n );
     auto i = nn.begin();
     auto e = nn.end();
     while( i != e ) {
       m_search_queue[i->id] = i->ep;
       ++i;
     }

     m_pending.reserve(m_p);
     fc::shared_ptr<kad_search> self(this,true);
     for( uint32_t i = 0; i < m_p ; ++i ) {
        m_pending.push_back( m_node->get_thread().async([=](){ self->search_thread(); }) );
     }
  }

  void kad_search::wait( const fc::microseconds& d ) {
    if( d == fc::microseconds::max() ) { 
        for( uint32_t i = 0; i < m_pending.size(); ++i )  {
          slog( "waiting... %d", i );
          m_pending[i].wait();
        }
    } else {
        auto timeout_time = fc::time_point::now() + d;
        for( uint32_t i = 0; i < m_pending.size(); ++i )  {
          slog( "waiting... %d", i );
          m_pending[i].wait_until( timeout_time );
        }
    }
  }

  /**
   *  This method is multi-plexed among multiple coroutines, and exits when the
   *  search queue is empty, the desired ID is found, or the search is
   *  canceled.  The search queue is empty once all nodes in the
   *  search path are included in the result set.
   *
   *  The search only gets narrower, it does not add nodes further away than the
   *  farthest result once the maximum number of results have been found.
   */
  void kad_search::search_thread() {
    slog( "search thread.... queue size %d", m_search_queue.size() );
    while( m_search_queue.size() && m_cur_status == kad_search::searching ) {
        fc::ip::endpoint ep  = m_search_queue.begin()->second;
        fc::sha1  nid        = m_search_queue.begin()->first ^ m_target;
        m_search_queue.erase(m_search_queue.begin());
        
        try {
          fc::sha1  rtn    = m_node->connect_to(ep);
          slog( "node %s found at %s", fc::string(rtn).c_str(), fc::string(ep).c_str() );

          // This filter may involve RPC calls.... 
          filter( rtn );
          slog( "    adding node %s to result list", fc::string(rtn).c_str() );
          m_current_results[m_target^rtn] = rtn;
          if( m_current_results.size() > m_n )  {
            m_current_results.erase( --m_current_results.end() );
          }

          if( rtn == m_target ) {
            m_cur_status = kad_search::done;
          }

          if( m_cur_status == kad_search::done )
            return;
          
          /** Only place the node in the search queue if it is closer than
             the furthest result.   If we are searching for 20 nodes and 
             already have 20 valid results, we only want the closest 20 and
             thus there is no need to consider a result further away.

             There is no need for the remote node to return nodes further away than our
             current 'worst result'.  Otherwise, we are consuming unecesary/redunant 
             bandwidth and ultimately searching almost every node on the network.
          */
          fc::optional<fc::sha1> limit;
          if( m_current_results.size() >= m_n && m_n ) {
              slog( "result size %d > target size %d", m_current_results.size(), m_n );
              limit = (--m_current_results.end())->first; 
          }

          slog( "finding %d nodes known by %s near target %s within limit %s  sqsize: %d", 
                  m_n, 
                  fc::string(rtn).c_str(), 
                  fc::string(m_target).c_str(), 
                  !!limit ? fc::string(*limit).c_str() : "_none_", 
                  m_search_queue.size() 
                  );
          auto rr =  m_node->remote_nodes_near( rtn, m_target, m_n, limit );
          auto rri = rr.begin();
          while( rri != rr.end() ) {
            // if the node is not in the current results 
            if( m_current_results.find( rri->id ) == m_current_results.end() ) {
              // if current results is not 'full' or the new result is less than the last
              // current result
              if( m_current_results.size() < m_n ) {
                m_search_queue[rri->id] = rri->ep;
              } else { // assume m_current_results.size() > 1 because m_n >= 1
                std::map<fc::sha1,fc::sha1>::const_iterator ritr = m_current_results.end();
                --ritr;
                if( ritr->first > rri->id ) { // only search the node if it is closer than current results
                  m_search_queue[rri->id] = rri->ep;
                }
              }
            }
            ++rri;
          }

        } catch ( ... ) {
          wlog( "%s", fc::current_exception().diagnostic_information().c_str() );
        }
    }
  }

  const fc::sha1& kad_search::target()const { return m_target; }

} // namespace tn


/*
  void node_private::kad_try_connection( const kad_search_state::ptr& kss, const connection::ptr& c ) {
    try {
      std::map<node_id, fc::ip::endpoint> peers         = c->find_peers( kss->target, 20 );
      std::map<node_id, fc::ip::endpoint>::iterator itr = peers.begin();
      while( itr != peers.end() ) {
        kss->search_queue[itr->first ^ target] = itr->second;
        ++itr;
      }
      while( peers.size() ) {
        fc::ip::endpoint ep = peers.front().second;
        peers.erase(peers.begin());
        node_id r = connect_to( ep );
        if( r == kss->target ) {
          kss->result.set_value( get_connection( r ) ); 
          return;
        } else {

        }
      }
    } catch ( const boost::exception& e ) {
      wlog( "Unexpected exception %1%", boost::diagnostic_information(e) );
    }
  }

  connection::ptr node_private::kad_find( const node_id& nid ) {
    node_id dist = nid ^ m_id;
    std::map<node_id,connection*>::iterator itr = m_dist_to_con.lower_bound( dist );
    if( itr->first == dist ) { return itr->second->shared_from_this(); }

    if( itr != m_dist_to_con.end() ) {
      // query K closest nodes to nid from itr, itr + 1, and itr + 2
      // when the result from any of those parallel queries returns, add the
      //  K closest to our list and then query the next closest.
      //  Stop when we have no closer nodes or when we find the node
      //  we are looking for.

      boost::shared_ptr<kad_search_state> kss( new kad_search_state() );
      kss->target      = nid;
      kss->target_dist = dist;
      return kss->wait();
    }

    return connection::ptr();
  }
  */