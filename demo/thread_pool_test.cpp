#include "ctpl_stl.h"
#include <vector>
#include <iostream>
#include <unistd.h>

int main (int argc, char *argv[]) {
    ctpl::thread_pool p(2 /* two threads in the pool */);
    // int arr[4] = {0};
    std::vector<int> arr(4);
    std::vector<std::future<void>> results(4);
    for (int i = 0; i < 8; ++i) { // for 8 iterations,
        for (int j = 0; j < 4; ++j) {
          results[j] = p.push([&arr, j](int id) { sleep(1); std::cout << "hello " << j << " from " << id << "\n"; });
        }
    }
}