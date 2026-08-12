# tools/setup_deps.ps1 が自動生成。手で編集しないこと。
$env:Eigen3_DIR = 'C:\Users\azoo\git\haori-server\external\thirdparty\eigen3\share\eigen3\cmake'
$env:TBB_DIR    = 'C:\Users\azoo\git\haori-server\external\thirdparty\oneapi-tbb-2021.12.0\lib\cmake\tbb'
$env:embree_DIR = 'C:\Users\azoo\git\haori-server\external\thirdparty\embree-3.13.1.x64.vc14.windows\lib\cmake\embree-3.13.1'
$env:PATH       = 'C:\Users\azoo\git\haori-server\external\thirdparty\embree-3.13.1.x64.vc14.windows\bin' + ';' + $env:PATH
