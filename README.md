# 개요
MIT에서 개발한 유닉스 계열 교육용 운영체제로, 멀티프로세서 x86 시스템을 기반으로 하는 XV6를 이용하여 운영체제와 관련된 여러 프로젝트를 진행합니다.

# 목차
- Project 1: Booting XV6 systems
- Project 2: System Call
- Project 3: CPU Scheduling
- Project 4: Virtual Memory
- Project 5: Page Replacement

# 내용
- Project 1: Booting XV6 systems
  - 설명: xv6를 초기 세팅한다.
  - 구현 내용:
    - init.c: booting시 학번 출력
    
- Project 2: System Call
  - 설명: getnice, setnice, ps라는 새로운 system call 함수를 만든다.
  - 구현 내용:
    - sysproc.c
      - sys_getname
      - sys_setnice
      - sys_ps
        
    - proc.c:
      - getname: name을 가져오는 함수
      - setnice: 프로세스의 nice 지수를 설정하는 함수
      - ps: 프로세스의 모든 정보를 출력하는 함수
    
- Project 3: CPU Scheduling
  - 설명: 기존 Round-Robin Scheduling 방식을 Completely Fair Scheduling 방식으로 변경한다.
  - 구현 내용:        
    - proc.c:
      - userinit: 프로세스 초기화시 priority, weight, vruntime, timeslice 초기화하도록 수정
      - fork: fork시 부모 프로세스의 위 속성들 복사하도록 수정
      - sheduler: cfs 스케줄러 구현
      - wakeup1: wakeup 시 위 속성 설정
        
    - trap.c
      - trap: CFS 스케줄링에 맞게 trap이 발생했을 경우 weight와 timeslice 조절
    
- Project 4: Virtual Memory
  - 설명: Virtual Memory를 관리하는 mmap, munmmap system call 함수와 page fault시 처리하는 page fault handler를 구현한다.
    
- Project 5: Page Replacement
  - 설명: LRU 방식으로 페이지 교체 알고리즘을 구현한다.
