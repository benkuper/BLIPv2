(module
  (import "blip" "probe" (func $probe (param i32) (result i32)))
  (memory (export "memory") 1 1)
  (func (export "noop") (param i32) (result i32) local.get 0)
  (func (export "integer") (param $n i32) (result i32) (local $v i32)
    i32.const 42 local.set $v
    (block $done (loop $again
      local.get $n i32.eqz br_if $done
      local.get $v i32.const 1664525 i32.mul i32.const 1013904223 i32.add local.set $v
      local.get $n i32.const 1 i32.sub local.set $n br $again)) local.get $v)
  (func (export "float") (param $n i32) (result f32) (local $v f32)
    (block $done (loop $again
      local.get $n i32.eqz br_if $done
      local.get $v local.get $n i32.const 1 i32.sub i32.const 255 i32.and f32.convert_i32_u
      f32.const 0.125 f32.mul f32.add local.set $v
      local.get $n i32.const 1 i32.sub local.set $n br $again)) local.get $v)
  (func (export "pixels") (param $n i32) (result i32) (local $i i32) (local $rgb i32) (local $sum i32)
    (block $done (loop $again
      local.get $i local.get $n i32.ge_u br_if $done
      local.get $i i32.const 255 i32.and i32.const 16 i32.shl
      local.get $i i32.const 3 i32.mul i32.const 255 i32.and i32.const 8 i32.shl i32.or
      local.get $i i32.const 7 i32.mul i32.const 255 i32.and i32.or local.set $rgb
      local.get $i i32.const 4 i32.mul local.get $rgb i32.store
      local.get $sum local.get $rgb i32.add local.set $sum
      local.get $i i32.const 1 i32.add local.set $i br $again)) local.get $sum)
  (func (export "host_calls") (param $n i32) (result i32) (local $v i32)
    (block $done (loop $again
      local.get $n i32.eqz br_if $done
      local.get $v local.get $n call $probe i32.add local.set $v
      local.get $n i32.const 1 i32.sub local.set $n br $again)) local.get $v)
  (func (export "trap") (param i32) (result i32) unreachable)
  (func (export "invalid") (param i32) (result i32) i32.const 65535 i32.load)
  (func (export "divide") (param i32) (result i32) i32.const 1 i32.const 0 i32.div_u)
  (func $recursive (export "recursion") (param i32) (result i32) local.get 0 call $recursive i32.const 1 i32.add)
  (func (export "grow") (param i32) (result i32) i32.const 1 memory.grow)
  (func (export "spin") (param i32) (result i32) (loop $again br $again) i32.const 0))
