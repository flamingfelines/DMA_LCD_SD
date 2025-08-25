import time
import gc
from tft_config import config, SCREEN_WIDTH, SCREEN_HEIGHT, set_backlight

set_backlight(1)

import time
import gc
from tft_config import config, SCREEN_WIDTH, SCREEN_HEIGHT

def dma_diagnostic_test():
    """Test to verify if DMA transfers are working properly"""
    
    display = config["display"]
    if not display:
        print("Display not initialized!")
        return
    
    buffer_size = SCREEN_WIDTH * SCREEN_HEIGHT * 2  # RGB565
    print("="*60)
    print("DMA TRANSFER DIAGNOSTIC TEST")
    print("="*60)
    print(f"Buffer size: {buffer_size} bytes ({buffer_size/1024:.1f} KB)")
    print()
    
    # Create different sized buffers to test transfer patterns
    test_sizes = [
        (60, 60, "Small buffer (7.2KB)"),
        (120, 120, "Medium buffer (28.8KB)"),  
        (240, 60, "Horizontal strip (28.8KB)"),
        (60, 240, "Vertical strip (28.8KB)"),
        (240, 240, "Full screen (115.2KB)")
    ]
    
    results = []
    
    for width, height, description in test_sizes:
        print(f"Testing: {description}")
        print("-" * 40)
        
        # Create test buffer for this size
        test_buffer_size = width * height * 2
        test_buffer = bytearray(test_buffer_size)
        
        # Fill with gradient pattern (more complex than solid color)
        for y in range(height):
            for x in range(width):
                # Create a gradient pattern
                r = (x * 31) // width
                g = (y * 63) // height  
                b = ((x + y) * 31) // (width + height)
                color = (r << 11) | (g << 5) | b
                
                pixel_index = (y * width + x) * 2
                test_buffer[pixel_index] = color & 0xFF
                test_buffer[pixel_index + 1] = (color >> 8) & 0xFF
        
        # Test transfer performance
        gc.collect()
        transfer_times = []
        
        # Do multiple transfers to get stable measurements
        for i in range(20):
            transfer_start = time.ticks_us()
            
            # Clear screen first (black)
            display.blit_buffer(bytearray(SCREEN_WIDTH * SCREEN_HEIGHT * 2), 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT)
            display.show()
            
            mid_time = time.ticks_us()
            
            # Transfer our test pattern
            display.blit_buffer(memoryview(test_buffer), 0, 0, width, height)
            display.show()
            
            transfer_end = time.ticks_us()
            
            clear_time = time.ticks_diff(mid_time, transfer_start)
            pattern_time = time.ticks_diff(transfer_end, mid_time)
            total_time = time.ticks_diff(transfer_end, transfer_start)
            
            transfer_times.append({
                'clear': clear_time,
                'pattern': pattern_time,
                'total': total_time
            })
        
        # Calculate statistics
        avg_clear = sum(t['clear'] for t in transfer_times) / len(transfer_times)
        avg_pattern = sum(t['pattern'] for t in transfer_times) / len(transfer_times)
        avg_total = sum(t['total'] for t in transfer_times) / len(transfer_times)
        
        min_total = min(t['total'] for t in transfer_times)
        max_total = max(t['total'] for t in transfer_times)
        
        # Calculate transfer rates
        bytes_per_second = test_buffer_size / (avg_pattern / 1_000_000)
        mbps = bytes_per_second / 1_000_000
        
        print(f"  Clear screen: {avg_clear/1000:.1f}ms")
        print(f"  Pattern transfer: {avg_pattern/1000:.1f}ms")
        print(f"  Total time: {avg_total/1000:.1f}ms")
        print(f"  Transfer rate: {mbps:.1f} MB/s")
        print(f"  Range: {min_total/1000:.1f}-{max_total/1000:.1f}ms")
        print()
        
        results.append({
            'description': description,
            'size_kb': test_buffer_size / 1024,
            'transfer_ms': avg_pattern / 1000,
            'rate_mbps': mbps
        })
    
    # Analysis
    print("="*60)
    print("TRANSFER ANALYSIS")
    print("="*60)
    
    # Check if transfer time scales linearly with buffer size
    print("Transfer time vs buffer size:")
    for result in results:
        ms_per_kb = result['transfer_ms'] / result['size_kb']
        print(f"  {result['description']:25s}: {ms_per_kb:.2f} ms/KB")
    
    print()
    
    # DMA diagnostic checks
    full_screen = next(r for r in results if "Full screen" in r['description'])
    small_buffer = next(r for r in results if "Small buffer" in r['description'])
    
    print("DMA DIAGNOSTIC CHECKS:")
    print("-" * 30)
    
    # Check 1: Is full screen transfer reasonable?
    if full_screen['transfer_ms'] > 30:
        print("⚠ ISSUE: Full screen transfer too slow (>30ms)")
        print("  Possible causes: Low SPI clock, no DMA, blocking transfers")
    else:
        print("✓ Full screen transfer time reasonable")
    
    # Check 2: Does transfer time scale linearly?
    expected_ratio = full_screen['size_kb'] / small_buffer['size_kb']
    actual_ratio = full_screen['transfer_ms'] / small_buffer['transfer_ms']
    ratio_difference = abs(expected_ratio - actual_ratio) / expected_ratio
    
    if ratio_difference > 0.3:  # >30% difference
        print("⚠ ISSUE: Transfer time doesn't scale linearly")
        print(f"  Expected ratio: {expected_ratio:.1f}x, Actual: {actual_ratio:.1f}x")
        print("  Possible cause: Setup overhead dominates small transfers")
    else:
        print("✓ Transfer time scales approximately linearly")
    
    # Check 3: Transfer rate consistency
    rates = [r['rate_mbps'] for r in results]
    rate_variation = (max(rates) - min(rates)) / max(rates)
    
    if rate_variation > 0.5:  # >50% variation
        print("⚠ ISSUE: Inconsistent transfer rates across buffer sizes")
        print("  Possible cause: DMA not working properly for all sizes")
    else:
        print("✓ Transfer rates relatively consistent")
    
    # Check 4: Absolute speed check
    max_rate = max(rates)
    if max_rate < 2.0:  # Less than 2 MB/s is very slow
        print(f"⚠ ISSUE: Very slow transfer rate ({max_rate:.1f} MB/s)")
        print("  Expected: 5-15 MB/s with proper DMA")
    elif max_rate < 5.0:
        print(f"⚠ ISSUE: Slow transfer rate ({max_rate:.1f} MB/s)")  
        print("  Expected: 5-15 MB/s with proper DMA")
    else:
        print(f"✓ Good transfer rate ({max_rate:.1f} MB/s)")
    
    print()
    print("THEORETICAL CALCULATIONS:")
    print("-" * 30)
    
    # Calculate theoretical rates at different SPI speeds
    spi_speeds = [10, 20, 40, 80]  # MHz
    for speed_mhz in spi_speeds:
        theoretical_bps = speed_mhz * 1_000_000 / 8  # Convert to bytes/second
        theoretical_mbps = theoretical_bps / 1_000_000
        full_screen_time_ms = (full_screen['size_kb'] * 1024) / theoretical_bps * 1000
        print(f"  At {speed_mhz:2d}MHz SPI: {theoretical_mbps:.1f} MB/s, full screen in {full_screen_time_ms:.1f}ms")
    
    print()
    your_rate = full_screen['rate_mbps']
    estimated_spi_speed = your_rate * 8  # Very rough estimate
    print(f"Your actual rate ({your_rate:.1f} MB/s) suggests ~{estimated_spi_speed:.0f}MHz effective speed")
    print("(Note: This is rough estimate, actual speed depends on many factors)")

def quick_dma_test():
    """Quick test to check basic DMA functionality"""
    print("Quick DMA functionality test...")
    
    display = config["display"]
    
    # Test 1: Very small transfer (should be fast)
    small_buffer = bytearray(100)  # 50 pixels
    start = time.ticks_us()
    display.blit_buffer(memoryview(small_buffer), 0, 0, 10, 5)
    display.show()
    small_time = time.ticks_diff(time.ticks_us(), start)
    
    # Test 2: Full screen transfer  
    full_buffer = bytearray(SCREEN_WIDTH * SCREEN_HEIGHT * 2)
    start = time.ticks_us()
    display.blit_buffer(memoryview(full_buffer), 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT)
    display.show()
    full_time = time.ticks_diff(time.ticks_us(), start)
    
    print(f"Small transfer (50 pixels): {small_time/1000:.1f}ms")
    print(f"Full screen (57,600 pixels): {full_time/1000:.1f}ms")
    print(f"Ratio: {full_time/small_time:.1f}x (should be ~1152x if purely size-dependent)")
    
    if small_time > 10000:  # >10ms for tiny transfer
        print("⚠ Even small transfers are slow - likely not using DMA")
    elif full_time / small_time < 100:  # Much less scaling than expected
        print("⚠ Poor scaling suggests setup overhead dominates")
    else:
        print("✓ Transfer scaling looks reasonable")

if __name__ == "__main__":
    dma_diagnostic_test()