// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

main(): Int64 {
    return run()

}

func run(): Int64 {
   var day :Right = Right()
   print(Right.Monday(1,3).toString()) // 231
   return 0
}

class Wrong{
    var a:Int32 = 1
    var b:String = #"xyz\nxyz"#
    var c:String = """
    lmn\nlmn"""
    class Monday{
        var a:Int32 = 1
        func monday(a:Int32,b!:Int32=10):Int32{
            return a+b
        }
        class Tuesday{
             var a:Int32 =1
             func tuesday(a:Int32,b!:Int32=1):Int32{
                 return (a+b)*2
             }
             class Wednesday{
                 var a:Int32 =1
                 func wednesday(a:Int32,b!:Int32=1):Int32{
                     return (a+b)*3
                 }
                 class Thursday{
                     var a:Int32 =1
                     func thurday(a:Int32,b!:Int32=1):Int32{
                         return (a+b)*4
                     }
                     class Friday{
                         var a:Int32 =1
                         func friday(a:Int32,b!:Int32=1):Int32{
                             return (a+b)*5
                         }
                         class Saturday{
                             var a:Int32 =1
                             func saturday(a:Int32,b!:Int32=1):Int32{
                                 return (a+b)*6
                             }
                             class Sunday{
                                  var a:Int32 =1
                                  func sunday(a:Int32,b!:Int32=1):Int32{
                                      func sunda(a:Int32,b!:Int32=1):Int32{
                                          func sund(a:Int32,b!:Int32=1){
                                              func sun(a:Int32,b!:Int32=1):Int32{
                                                  func su(a:Int32,b!:Int32=1){
                                                      var c:Int32 = (a+b)*11
                                                      return c
                                                  }
                                                  var c:Int32 = (a+b)*10 + su(a)
                                                  return c
                                              }
                                              var c:Int32 = (a+b)*9 + sun(a)
                                              return c
                                          }
                                          var c:Int32 = (a+b)*8 + sund(a)
                                          return c
                                      }
                                      var c:Int32 = (a+b)*7 + sunda(a)
                                      return c
                                  }
                                  var b: Int32 = 2
                              }
                              var b: Int32 = 2
                         }
                         var b: Int32 = 2
                       }
                       var b: Int32 = 2
                  }
                  var b: Int32 = 2
             }
             var b: Int32 = 2
        }
        var b: Int32 = 2
    }
    var d: Int32 = 2
}


class Right{
    var a:Int32 = 1
    var b:String = #"xyz\nxyz"#
    var c:String = """
    lmn\nlmn"""
    static func Monday(a:Int32,b:Int32):Int32{
        var m:Int32 = a*2
        var n:Int32 = b*2
        return Tuesday(m,b:a,a:b)
    }
    static func Tuesday(o:Int32,a!:Int32=2,b!:Int32=3):Int32{
        return o*100+a*10+b
    }

}



