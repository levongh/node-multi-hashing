#pragma once

#include "nrghash.h"
#include <memory>
#include <mutex>
#include <iostream>
#include <iomanip>
#include <atomic>
#include <thread>

const uint64_t nextDagGenerationDistance = 400;
static std::atomic_bool generationThreadBusy(false);

inline const std::unique_ptr<n_nrghash::dag_t>& ActiveDAG(
        std::unique_ptr<n_nrghash::dag_t> next_dag = std::unique_ptr<n_nrghash::dag_t>(),
        bool reset = false)
{
    using namespace std;

    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    static unique_ptr<n_nrghash::dag_t> active; // only keep one DAG in memory at once

    if (reset && active) {
        active->unload();
        active.reset();
    }

    // if we have a next_dag swap it
    if (next_dag) {
        auto const previous_epoch = active ? active->epoch() : 0;
        auto const new_epoch = next_dag->epoch();

        active.swap(next_dag);
        if (new_epoch != previous_epoch) {
            std::cout << "nrghash DAG swapped to new epoch "
                      << previous_epoch << "->" << new_epoch << std::endl;
        } else {
            std::cout <<"nrghash DAG activated for epoch " << new_epoch << std::endl;
        }
        if (next_dag) {
            next_dag->unload();
            next_dag.reset();
        }
        std::cout << "nrghash DAG for epoch " << previous_epoch << " unloaded\n";
    }
    return active;
}

std::string GetDataDir()
{
    std::string result;
#ifdef WIN32
    return std::string(getenv("APPDATA")) + std::string("/EnergiCore/miningpool");
#else
    char* homePath = getenv("HOME");
    if (homePath == nullptr || strlen(homePath) == 0) {
        result = std::string("/");
    } else {
        result = std::string(homePath);
    }
#ifdef MAC_OSX
    return result + std::string("Library/Application Support/EnergiCore/miningpool");
#else
    return result + std::string("/.energicore/miningpool/");
#endif
#endif
}

inline void GenerateNewDAG(int height, std::string epoch_file)
{
    generationThreadBusy = true;
    using namespace n_nrghash;

    // no callback
    try {
        dag_t::generateAndSave(height, epoch_file);
    } catch (hash_exception const & e) {
        std::cout << "DAG for " << height / constants::EPOCH_LENGTH
                  << " could not be generated: " << e.what();
    }
    generationThreadBusy = false;
}

void CreateDAG(int height, n_nrghash::progress_callback_type callback = [](n_nrghash::dag_t::size_type, n_nrghash::dag_t::size_type, int){ return true; })
{
    using namespace std;
    using namespace n_nrghash;
    static std::thread* next_dag_generation_thread = nullptr;

    auto const epoch = height / constants::EPOCH_LENGTH;
    auto const & seedhash = cache_t::get_seedhash(height).to_hex();
    stringstream ss;
    ss << hex << setw(4) << setfill('0') << epoch << "-" << seedhash.substr(0, 12) << ".dag";

    auto const epoch_file = GetDataDir() + std::string("dag/") + ss.str();

    std::cout << "\nDAG file for epoch " << epoch << " is " << epoch_file << std::endl;

    if (!dag_t::is_dag_file_corrupted(epoch_file)) {
        // if file exists and looks ok, then don't generate the same epoch file again
        return;
    }

    // try to generate the DAG
    try {
        auto const & dag = ActiveDAG();
        auto res = system(std::string(std::string("mkdir -p ") + GetDataDir() + std::string("dag/")).c_str());
        (void)res;
        if (dag) {
            if (generationThreadBusy) {
                // already generating in another dag generation thread
                return;
            } else {
                if (next_dag_generation_thread) {
                    delete next_dag_generation_thread;
                    next_dag_generation_thread = nullptr;
                }
            }
            // if there is already an active dag generate the other with low memory in another thread
            next_dag_generation_thread  = new std::thread(GenerateNewDAG, height, epoch_file);
        }  else {
            // if no dag is generated or loaded, generate in normal mode
            unique_ptr<dag_t> new_dag = unique_ptr<dag_t>(new dag_t(height, callback));
            new_dag->save(epoch_file);
            ActiveDAG(move(new_dag));
            std::cout << "\nDAG generated successfully. Saved to " << epoch_file << std::endl;
        }
    } catch (hash_exception const & e) {
        std::cout << "\nDAG for epoch " << epoch << " could not be generated: " << e.what() << std::endl;
    }
}

bool LoadDAG(int height,
             n_nrghash::progress_callback_type callback = [](
             n_nrghash::dag_t::size_type,
             n_nrghash::dag_t::size_type,
             int){ return true; })
{
    using namespace std;
    using namespace n_nrghash;

    auto const epoch = height / constants::EPOCH_LENGTH;
    auto const & seedhash = cache_t::get_seedhash(height).to_hex();
    stringstream ss;
    ss << hex << setw(4) << setfill('0') << epoch << "-" << seedhash.substr(0, 12) << ".dag";
    auto const epoch_file = GetDataDir() + std::string("dag/") + ss.str();


    std::cout << "nrghash DAG file for epoch " << epoch << " is " << epoch_file << std::endl;
    if (dag_t::is_loaded(epoch)) {
        // already loaded
        return true;
    }

    // try to load the DAG from disk
    try  {
        if (ActiveDAG())  {
            // force dag reset
            ActiveDAG(nullptr, true);
        }
        unique_ptr<dag_t> new_dag(new dag_t(epoch_file, callback));
        ActiveDAG(move(new_dag));
        std::cout << "nrghash DAG file " << epoch_file << " loaded successfully.\n";;
        return true;
    } catch (hash_exception const & e) {
        std::cout << "nrghash DAG file " << epoch_file
                  << " not loaded, will be generated instead. Message: " <<e.what();
    }
    return false;
}

inline bool LoadNrgHashDAG(uint64_t blockHeight, n_nrghash::progress_callback_type callback)
{
    // initialize the DAG
    if (!LoadDAG(blockHeight, callback)) {
        CreateDAG(blockHeight, callback);
        return true;
    }
    return false;
}

